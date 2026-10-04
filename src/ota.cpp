#include "ota.h"

#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include "secrets.h"

bool Ota::begin() {
   return true;
}

bool Ota::run() {
   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   WiFiClientSecure client;
   client.setInsecure();

   t_httpUpdate_return result =
      ESPhttpUpdate.update(client, OTA_FIRMWARE_URL);

   switch (result) {
      case HTTP_UPDATE_FAILED:
         Serial.printf(
            "OTA falhou: (%d) %s\n",
            ESPhttpUpdate.getLastError(),
            ESPhttpUpdate.getLastErrorString().c_str()
         );
         return false;

      case HTTP_UPDATE_NO_UPDATES:
         Serial.println("OTA: nenhuma atualização disponível.");
         return false;

      case HTTP_UPDATE_OK:
         // Normally does not return: ESP8266HTTPUpdate reboots after success.
         Serial.println("OTA concluída.");
         return true;
   }

   return false;
}
