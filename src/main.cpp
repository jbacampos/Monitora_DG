#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
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

      enqueue(current, ts);
      persisted.power = current;

      if (redeChanged) {
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

      if (!telemetry.publishEvent(record)) {
         return;
      }

      persisted.tbLogHead++;

      if (persisted.tbLogHead >= tbQueue.count() || (persisted.tbLogHead - lastSavedHead) >= TB_HEAD_SAVE_EVERY) {
         lastSavedHead = persisted.tbLogHead;
         saveState();
      }
   }

   void onTimeSynced() {
      Serial.println("NTP sincronizado.");

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
      telegram.update(now);

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

   if (!stateStore.begin()) {
      Serial.println("ERRO: LittleFS");
   }

   tbQueue.begin();
   tbQueue.invalidateSessionTimestamps();

   const bool haveState = stateStore.load(persisted);

   inputs.begin();
   timeSource.begin();

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

   wifi.begin();
   telegram.begin();
   notifier.begin(&telegram, &stateStore, &persisted);
   telemetry.begin();
   ota.begin();

   reconcileBoot();

   lastSample = millis();
   lastAlive = millis();
   lastSavedHead = persisted.tbLogHead;

   printState("Estado inicial:", persisted.power);
   Serial.printf("Pendentes TB: %lu\n", static_cast<unsigned long>(tbQueue.count() - persisted.tbLogHead));
}

void loop() {
   const uint32_t now = millis();

   wifi.update(now);

   if (timeSource.update()) {
      onTimeSynced();
   }

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

   telemetry.update(now);

   const uint32_t aliveEpoch = timeSource.synced() ? timeSource.now().value : 0;
   telemetry.publishHeartbeat(now, aliveEpoch);
   publishPendingTb();

   notifier.update();
   handleTelegramCommands(now);

   updateAlive(now);
   printDiagnostics(now);

   yield();
}