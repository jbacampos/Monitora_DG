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

   // GitHub does not serve the asset directly: /releases/latest/download/... answers 302
   // twice (github.com -> .../download/<tag>/... -> release-assets.githubusercontent.com)
   // and ESP8266HTTPUpdate disables redirect following by default, so the update could
   // never start. STRICT is enough: the redirects are 302 and the method is GET.
   ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

   // This client keeps the BearSSL default buffers: 16384 + 325 = 16709 B of CONTIGUOUS
   // RAM (~16.7 kB). Shrinking them with MFLN was tried and failed: the MFLN probe only
   // covers the URL host (github.com), while GitHub answers 302 and the body is served by
   // release-assets.githubusercontent.com, which does not negotiate MFLN - so a smaller
   // buffer ended with BR_ERR_TOO_LARGE (incoming record larger than the buffer) after
   // ~44 kB. main.cpp therefore runs ota.run() from runPendingOtaIfRequested() on a clean
   // boot, before Telegram and ThingsBoard exist, so the default buffer fits.
   //
   // Operational diagnostic: heap right before the OTA client connects.
   Serial.printf("OTA HEAP: free=%u maxblock=%u\n", ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());

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
