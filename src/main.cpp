#include <Arduino.h>

#include "config.h"
#include "input_monitor.h"
#include "secrets.h"
#include "state_store.h"
#include "tb_queue.h"
#include "time_source.h"
#include "types.h"
#include "wifi_manager.h"

namespace {
   WiFiManager wifi;
   TimeSource timeSource;
   InputMonitor inputs;
   StateStore stateStore;
   TbQueue tbQueue;

   PersistedState persisted{};

   uint32_t lastSample = 0;
   uint32_t lastAlive = 0;

   void saveState() {
      stateStore.save(persisted);
   }

   // Persist one historical snapshot for ThingsBoard (append-only log).
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
      // The event is persisted BEFORE any network operation.
      enqueue(current, ts);
      persisted.power = current;
      saveState();
   }

   void reconcileBoot() {
      const PowerState actual = inputs.state();

      if (!persisted.validReliable) {
         // First boot: never invent FALTA or RETORNO.
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

      if (before && !after) {
         // CASE B: outage happened while the ESP was still running.
         enqueue(actual, persisted.alive.timestamp);
      } else if (!before && after) {
         // CASE C: RETORNO detected at boot.
         enqueue(actual, timeSource.now());
      }

      // CASE A and CASE D create no reconstruction event.

      persisted.power = actual;

      if (actual.redeDisponivel) {
         persisted.alive.timestamp = timeSource.now();
         persisted.alive.redeDisponivel = true;
      }

      // Mandatory boot snapshot, always after any reconstruction event.
      enqueue(actual, timeSource.now());
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

   void printState(const char *prefix, const PowerState &s) {
      Serial.printf("%s rede=%d alim_rede=%d offgrid=%d gerador=%d\n", prefix, s.redeDisponivel, s.alimentacaoRede, s.alimentacaoOffgrid, s.alimentacaoGerador);
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

   // Every record found at boot belongs to a previous session.
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

   reconcileBoot();

   lastSample = millis();
   lastAlive = millis();

   printState("Estado inicial:", persisted.power);
   Serial.printf("Pendentes TB: %lu\n", static_cast<unsigned long>(tbQueue.count() - persisted.tbLogHead));
}

void loop() {
   const uint32_t now = millis();

   wifi.update(now);
   timeSource.update();

   // GPIO monitoring has priority over every network operation.
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

   updateAlive(now);

   yield();
}