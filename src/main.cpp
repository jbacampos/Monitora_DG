#include <Arduino.h>

#include "config.h"
#include "input_monitor.h"
#include "secrets.h"
#include "time_source.h"
#include "types.h"
#include "wifi_manager.h"

namespace {
   WiFiManager wifi;
   TimeSource timeSource;
   InputMonitor inputs;

   uint32_t lastSample = 0;
}

void setup() {
   Serial.begin(115200);
   delay(50);

   Serial.println();
   Serial.println("Boot Monitora_DG");

   inputs.begin();
   timeSource.begin();
   wifi.begin();

   lastSample = millis();
}

void loop() {
   const uint32_t now = millis();

   wifi.update(now);
   timeSource.update();

   if ((now - lastSample) >= INPUT_SAMPLE_MS) {
      lastSample = now;
      inputs.update(now);

      if (inputs.changed()) {
         const PowerState &s = inputs.state();
         Serial.printf("Estado: rede=%d alim_rede=%d offgrid=%d gerador=%d\n", s.redeDisponivel, s.alimentacaoRede, s.alimentacaoOffgrid, s.alimentacaoGerador);
         inputs.clearChanged();
      }
   }

   yield();
}