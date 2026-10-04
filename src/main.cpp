#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"
#include "inputs.h"
#include "notifications.h"
#include "ota.h"
#include "secrets.h"
#include "state_store.h"
#include "telegram.h"
#include "telemetry.h"
#include "types.h"
#include "wifi_manager.h"

namespace {
   WiFiManager wifi;
   InputMonitor inputs;
   StateStore stateStore;
   Telegram telegram;
   Notifications notifications;
   Telemetry telemetry;
   Ota ota;

   PersistedState persisted{};
   PowerState lastObserved{};

   uint32_t lastInputSample = 0;
   bool havePersistedState = false;

   bool sameState(const PowerState &a, const PowerState &b) {
      return
         a.redeDisponivel == b.redeDisponivel &&
         a.alimentacaoRede == b.alimentacaoRede &&
         a.alimentacaoOffgrid == b.alimentacaoOffgrid &&
         a.alimentacaoGerador == b.alimentacaoGerador;
   }

   void makeInitialStateIfNeeded() {
      if (havePersistedState) {
         return;
      }

      persisted = {};
      persisted.magic = STATE_MAGIC;
      persisted.version = STATE_VERSION;
      persisted.power = inputs.state();
      persisted.lastFallStickerId = 0;

      stateStore.save(persisted);
      havePersistedState = true;
   }

   void reconcileStateAfterBoot() {
      const PowerState actual = inputs.state();

      if (!havePersistedState) {
         makeInitialStateIfNeeded();
         lastObserved = actual;
         return;
      }

      // The persisted state is the reference from before a reboot/power loss.
      // A difference in the Equatorial availability signal is therefore a
      // real state transition that occurred while the device was away.
      if (!sameState(persisted.power, actual)) {
         const PowerState previous = persisted.power;

         persisted.power = actual;
         stateStore.save(persisted);

         if (previous.redeDisponivel != actual.redeDisponivel) {
            notifications.onPowerStateChanged(previous, actual);
         }
      }

      lastObserved = actual;
   }

   void processStateChange() {
      const PowerState current = inputs.state();

      if (sameState(current, persisted.power)) {
         lastObserved = current;
         return;
      }

      const PowerState previous = persisted.power;

      // Persist before external I/O. If power disappears immediately after
      // the physical transition, the next boot still knows the new state.
      persisted.power = current;
      stateStore.save(persisted);

      lastObserved = current;

      if (previous.redeDisponivel != current.redeDisponivel) {
         notifications.onPowerStateChanged(previous, current);
      }

      // State telemetry is best-effort. The state itself is already safe in
      // LittleFS and will be published again at the next boot or heartbeat.
      telemetry.publishState(current);
   }

   void printBootInfo() {
      Serial.println();
      Serial.println("==============================");
      Serial.println("         MONITORA_DG");
      Serial.println("==============================");
      Serial.print("Firmware : ");
      Serial.println(FIRMWARE_VERSION);
      Serial.print("Device ID: ");
      Serial.println(DEVICE_ID);
      Serial.println("------------------------------");
      Serial.print("Rede disp: ");
      Serial.println(persisted.power.redeDisponivel);
      Serial.print("Alim rede: ");
      Serial.println(persisted.power.alimentacaoRede);
      Serial.print("Alim offgrid: ");
      Serial.println(persisted.power.alimentacaoOffgrid);
      Serial.print("Alim gerador: ");
      Serial.println(persisted.power.alimentacaoGerador);
      Serial.print("Sticker queda: ");
      Serial.println(persisted.lastFallStickerId);
      Serial.println("==============================");
   }
}

void setup() {
   Serial.begin(115200);
   delay(50);

   Serial.println();
   Serial.println("Boot Monitora_DG");

   if (!stateStore.begin()) {
      Serial.println("ERRO: LittleFS");
   }

   havePersistedState = stateStore.load(persisted);

   if (havePersistedState) {
      Serial.println("Estado persistente carregado.");
   } else {
      Serial.println("Nenhum estado persistente válido.");
   }

   inputs.begin();

   wifi.begin();

   telegram.begin();
   notifications.begin(&telegram, &persisted);

   telemetry.begin();
   ota.begin();

   makeInitialStateIfNeeded();
   reconcileStateAfterBoot();

   printBootInfo();

   // The current physical state is always published after boot when MQTT
   // becomes available. This does not generate a Telegram notification.
   lastInputSample = millis();
}

void loop() {
   const uint32_t now = millis();

   wifi.update(now);

   // GPIO monitoring has priority over network maintenance.
   if (static_cast<uint32_t>(now - lastInputSample) >= INPUT_SAMPLE_MS) {
      lastInputSample = now;

      inputs.update(now);

      if (inputs.changed()) {
         processStateChange();
         inputs.clearChanged();
      }
   }

   telemetry.update(now, inputs.state());
   telegram.update(now);

   if (telegram.commandOtaRequested()) {
      telegram.sendText("Comando /ota recebido. A rotina de OTA será executada.");
      ota.run();
   }

   if (telegram.commandRebootRequested()) {
      telegram.sendText("Comando /reboot recebido. Reiniciando.");
      delay(100);
      ESP.restart();
   }

   yield();
}
