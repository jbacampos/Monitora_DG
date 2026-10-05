#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "heap_diag.h"
#include "input_monitor.h"
#include "ota.h"
#include "secrets.h"
#include "state_store.h"
#include "tb_queue.h"
#include "telegram.h"
#include "telegram_notifier.h"
#include "telemetry.h"
#include "time_source.h"
#include "types.h"
#include "wifi_manager.h"

namespace {
   WiFiManager wifi;
   TimeSource timeSource;
   InputMonitor inputs;
   StateStore stateStore;
   TbQueue tbQueue;
   Telemetry telemetry;
   Telegram telegram;
   TelegramNotifier notifier;
   Ota ota;

   PersistedState persisted{};

   uint32_t lastSample = 0;
   uint32_t lastAlive = 0;
   uint32_t lastSavedHead = 0;
   uint32_t lastDiag = 0;

   // Diagnostic-only: edge detector so the Wi-Fi connect instant is logged once.
   bool wifiConnectedLogged = false;

   void saveState() {
      stateStore.save(persisted);
   }

   void enqueue(const PowerState &power, const Timestamp &ts) {
      PendingRecord record;
      record.timestamp = ts;
      record.power = power;

      if (!tbQueue.append(record)) {
         persisted.tbGapCount++;
         Serial.println("AVISO: pending.bin sem espaco; evento nao registrado (diagnostico).");
      }
   }

   void handleTransition(const PowerState &current, const Timestamp &ts) {
      const bool redeChanged = current.redeDisponivel != persisted.power.redeDisponivel;
      const bool previousRede = persisted.power.redeDisponivel;

      enqueue(current, ts);
      persisted.power = current;

      if (redeChanged) {
         const uint32_t tNow = millis();

         Serial.printf("[%lu] TG: TRANSICAO rede=%d\n", static_cast<unsigned long>(tNow), current.redeDisponivel ? 1 : 0);
         Serial.printf("[%lu] TG: timestamp evento kind=%d value=%lu\n", static_cast<unsigned long>(tNow), static_cast<int>(ts.kind),
                       static_cast<unsigned long>(ts.value));
         Serial.printf("[%lu] TG: estado anterior=%d estado novo=%d\n", static_cast<unsigned long>(tNow), previousRede ? 1 : 0, current.redeDisponivel ? 1 : 0);

         notifier.onRedeTransition(ts);
      }

      saveState();
   }

   void reconcileBoot() {
      const PowerState actual = inputs.state();

      if (!persisted.validReliable) {
         persisted.power = actual;
         persisted.alive.timestamp = timeSource.now();
         persisted.alive.redeDisponivel = actual.redeDisponivel;
         persisted.tgNotifiedRede = actual.redeDisponivel;
         persisted.tgPending = false;
         persisted.tgForceNotify = false;
         persisted.tgPhase = TG_PHASE_NONE;
         persisted.validReliable = true;

         enqueue(actual, timeSource.now());
         saveState();
         return;
      }

      const bool before = persisted.power.redeDisponivel;
      const bool after = actual.redeDisponivel;

      Timestamp reconstructed{};
      bool hasReconstruction = false;
      bool reconstructReturn = false;

      if (before && !after) {
         reconstructed = persisted.alive.timestamp; // CASE B
         hasReconstruction = true;
      } else if (!before && after) {
         reconstructed = timeSource.now(); // CASE C
         hasReconstruction = true;
         reconstructReturn = true;
      }

      persisted.power = actual;

      if (hasReconstruction) {
         enqueue(actual, reconstructed);

         if (reconstructReturn) {
            notifier.onBootReturnReconstruction(reconstructed);
         } else {
            notifier.onRedeTransition(reconstructed);
         }
      }

      if (actual.redeDisponivel) {
         persisted.alive.timestamp = timeSource.now();
         persisted.alive.redeDisponivel = true;
      }

      enqueue(actual, timeSource.now()); // mandatory boot snapshot
      saveState();
   }

   void updateAlive(uint32_t now) {
      if (!persisted.power.redeDisponivel) {
         return;
      }

      if ((now - lastAlive) < ALIVE_INTERVAL_MS) {
         return;
      }

      lastAlive = now;
      persisted.alive.timestamp = timeSource.now();
      persisted.alive.redeDisponivel = true;
      saveState();
   }

   void publishPendingTb() {
      if (!telemetry.connected()) {
         return;
      }

      const uint32_t count = tbQueue.count();

      if (persisted.tbLogHead >= count) {
         if (count > 0) {
            tbQueue.reset();
            persisted.tbLogHead = 0;
            lastSavedHead = 0;
            saveState();
         }
         return;
      }

      PendingRecord record;

      if (!tbQueue.readAt(persisted.tbLogHead, record)) {
         return;
      }

      if (record.timestamp.kind == TsKind::SessionMillis) {
         return;
      }

      const uint32_t tbStart = millis();

      Serial.printf("[%lu] TB: publishPending inicio\n", static_cast<unsigned long>(tbStart));

      const bool published = telemetry.publishEvent(record);

      Serial.printf("[%lu] TB: publishPending fim duracao=%lums\n", static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis() - tbStart));

      if (!published) {
         Serial.printf("[%lu] TB: falha de publicacao\n", static_cast<unsigned long>(millis()));
         return;
      }

      Serial.printf("[%lu] TB: evento publicado\n", static_cast<unsigned long>(millis()));

      persisted.tbLogHead++;

      if (persisted.tbLogHead >= tbQueue.count() || (persisted.tbLogHead - lastSavedHead) >= TB_HEAD_SAVE_EVERY) {
         lastSavedHead = persisted.tbLogHead;
         saveState();
      }
   }

   void onTimeSynced() {
      Serial.println("NTP sincronizado.");
      heapDiag("evento:ntp-sincronizado");

      if (tbQueue.resolveSessionTimestamps(timeSource, persisted.tbLogHead)) {
         Serial.println("Log TB: timestamps de sessao convertidos para Epoch.");
      }

      bool changed = false;

      if (timeSource.resolve(persisted.alive.timestamp)) {
         changed = true;
      }

      if (timeSource.resolve(persisted.tgTimestamp)) {
         changed = true;
      }

      if (changed) {
         saveState();
      }
   }

   void handleTelegramCommands(uint32_t now) {
      // While a notification sequence is in flight (tgPending with an active
      // phase), its phases must run back to back: polling getUpdates() here would
      // insert a full HTTPS round-trip between every step. Commands are still
      // processed normally as soon as the sequence has finished (phase == NONE).
      const bool notifying = persisted.tgPending && persisted.tgPhase != TG_PHASE_NONE;

      if (!notifying) {
         telegram.update(now);
      }

      if (telegram.commandOtaRequested()) {
         Serial.println("Comando /ota recebido.");
         telegram.sendText("Comando /ota recebido. Executando atualizacao.");
         ota.run();
      }

      if (telegram.commandRebootRequested()) {
         Serial.println("Comando /reboot recebido.");
         telegram.sendText("Comando /reboot recebido. Reiniciando.");
         delay(100);
         ESP.restart();
      }
   }

   const char *phaseName(uint8_t phase) {
      switch (phase) {
         case TG_PHASE_NONE:
            return "none";
         case TG_PHASE_DEL_OLD:
            return "del_old";
         case TG_PHASE_SEND_FALL:
            return "send_fall";
         case TG_PHASE_TEXT_FALL:
            return "text_fall";
         case TG_PHASE_SEND_RET:
            return "send_ret";
         case TG_PHASE_TEXT_RET:
            return "text_ret";
         default:
            return "?";
      }
   }

   void printState(const char *prefix, const PowerState &s) {
      Serial.printf("%s rede=%d alim_rede=%d offgrid=%d gerador=%d\n", prefix, s.redeDisponivel, s.alimentacaoRede, s.alimentacaoOffgrid, s.alimentacaoGerador);
   }

   void printDiagnostics(uint32_t now) {
      if ((now - lastDiag) < DIAGNOSTIC_INTERVAL_MS) {
         return;
      }

      lastDiag = now;

      const uint32_t count = tbQueue.count();

      Serial.printf("DIAG up=%lus wifi=%d rssi=%d ntp=%d rede=%d pend=%lu head=%lu gap=%u tgPend=%d tgNotif=%d tgForce=%d tgPhase=%s fallId=%ld retId=%ld\n",
                    static_cast<unsigned long>(now / 1000UL), WiFi.status() == WL_CONNECTED ? 1 : 0, WiFi.RSSI(), timeSource.synced() ? 1 : 0,
                    persisted.power.redeDisponivel ? 1 : 0, static_cast<unsigned long>(count - persisted.tbLogHead),
                    static_cast<unsigned long>(persisted.tbLogHead), persisted.tbGapCount, persisted.tgPending ? 1 : 0, persisted.tgNotifiedRede ? 1 : 0,
                    persisted.tgForceNotify ? 1 : 0, phaseName(persisted.tgPhase), static_cast<long>(persisted.lastFallStickerId),
                    static_cast<long>(persisted.lastReturnStickerId));
   }
} // namespace

void setup() {
   Serial.begin(115200);
   delay(50);

   Serial.println();
   Serial.println("Boot Monitora_DG");

   heapDiag("setup:boot");

   if (!stateStore.begin()) {
      Serial.println("ERRO: LittleFS");
   }

   heapDiag("setup:pos-littlefs");

   tbQueue.begin();
   tbQueue.invalidateSessionTimestamps();

   heapDiag("setup:pos-tbqueue");

   const bool haveState = stateStore.load(persisted);

   inputs.begin();

   heapDiag("setup:pos-gpio");

   timeSource.begin();

   heapDiag("setup:pos-ntp-start");

   if (haveState) {
      const uint32_t count = tbQueue.count();

      if (persisted.tbLogHead > count) {
         persisted.tbLogHead = count;
      }

      if (persisted.alive.timestamp.kind == TsKind::SessionMillis) {
         persisted.alive.timestamp.kind = TsKind::LostSession;
      }

      if (persisted.tgTimestamp.kind == TsKind::SessionMillis) {
         persisted.tgTimestamp.kind = TsKind::LostSession;
      }

      Serial.println("Estado persistente carregado.");
   } else {
      Serial.println("Sem estado persistente (primeira inicializacao).");
   }

   heapDiag("setup:pos-load");

   wifi.begin();

   heapDiag("setup:pos-wifi-begin");

   telegram.begin();

   heapDiag("setup:pos-telegram-begin");

   notifier.begin(&telegram, &stateStore, &persisted);
   telemetry.begin();
   ota.begin();

   heapDiag("setup:pos-clients-begin");

   reconcileBoot();

   heapDiag("setup:pos-reconcile");

   lastSample = millis();
   lastAlive = millis();
   lastSavedHead = persisted.tbLogHead;

   printState("Estado inicial:", persisted.power);
   Serial.printf("Pendentes TB: %lu\n", static_cast<unsigned long>(tbQueue.count() - persisted.tbLogHead));

   heapDiag("setup:fim");
}

void loop() {
   const uint32_t now = millis();
   const uint32_t loopStart = now;

   wifi.update(now);

   // Diagnostic-only: log the Wi-Fi connect instant once (rising edge).
   if (!wifiConnectedLogged && WiFi.status() == WL_CONNECTED) {
      wifiConnectedLogged = true;
      heapDiag("evento:wifi-conectado");
   }

   const uint32_t afterWifi = millis();

   if (timeSource.update()) {
      onTimeSynced();
   }
   const uint32_t afterNtp = millis();

   if ((now - lastSample) >= INPUT_SAMPLE_MS) {
      lastSample = now;
      inputs.update(now);

      if (inputs.changed()) {
         const PowerState current = inputs.state();

         if (!samePowerState(current, persisted.power)) {
            handleTransition(current, timeSource.now());
            printState("Transicao:", current);
         }

         inputs.clearChanged();
      }
   }
   const uint32_t afterSample = millis();

   telemetry.update(now);

   const uint32_t aliveEpoch = timeSource.synced() ? timeSource.now().value : 0;
   telemetry.publishHeartbeat(now, aliveEpoch);
   publishPendingTb();
   const uint32_t afterTb = millis();

   notifier.update();
   handleTelegramCommands(now);
   const uint32_t afterTg = millis();

   updateAlive(now);
   printDiagnostics(now);
   const uint32_t afterMisc = millis();

   yield();
   const uint32_t loopEnd = millis();

   // Diagnostic-only: report abnormal cycles (and per-stage split) without
   // changing the ordering or the logic of the loop.
   const uint32_t loopDur = static_cast<uint32_t>(loopEnd - loopStart);

   if (loopDur >= 500) {
      Serial.printf("[%lu] LOOP: ciclo longo=%lums wifi=%lu ntp=%lu sample=%lu tb=%lu tg=%lu misc=%lu\n", static_cast<unsigned long>(loopEnd),
                    static_cast<unsigned long>(loopDur), static_cast<unsigned long>(afterWifi - loopStart), static_cast<unsigned long>(afterNtp - afterWifi),
                    static_cast<unsigned long>(afterSample - afterNtp), static_cast<unsigned long>(afterTb - afterSample),
                    static_cast<unsigned long>(afterTg - afterTb), static_cast<unsigned long>(afterMisc - afterTg));
   }
}