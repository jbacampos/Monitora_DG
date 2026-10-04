#include "wifi_manager.h"

#include <ESP8266WiFi.h>

#include "config.h"
#include "secrets.h"

namespace {
   uint32_t lastAttempt = 0;
}

void WiFiManager::begin() {
   WiFi.mode(WIFI_STA);
   WiFi.persistent(false);
   WiFi.setAutoReconnect(true);
   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

   lastAttempt = millis();
}

void WiFiManager::update(uint32_t now) {
   if (WiFi.status() == WL_CONNECTED) {
      return;
   }

   if (static_cast<uint32_t>(now - lastAttempt) < WIFI_RETRY_MS) {
      return;
   }

   lastAttempt = now;
   WiFi.disconnect(false);
   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool WiFiManager::connected() const {
   return WiFi.status() == WL_CONNECTED;
}
