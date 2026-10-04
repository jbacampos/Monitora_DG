#include "telemetry.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "secrets.h"

namespace {
   WiFiClientSecure tlsClient;
   PubSubClient mqtt(tlsClient);
}

bool Telemetry::begin() {
   tlsClient.setInsecure();
   tlsClient.setTimeout(1500);

   mqtt.setServer(TB_HOST, TB_PORT);
   mqtt.setBufferSize(512);

   connected_ = false;
   statePublished_ = false;
   lastMqttAttempt_ = 0;
   lastHeartbeat_ = 0;

   return true;
}

bool Telemetry::connectMqtt() {
   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   const String clientId =
      String(DEVICE_ID) + "-" + String(ESP.getChipId(), HEX);

   if (mqtt.connect(
          clientId.c_str(),
          TB_ACCESS_TOKEN,
          nullptr)) {
      connected_ = true;
      statePublished_ = false;
      return true;
   }

   connected_ = false;
   return false;
}

bool Telemetry::publishJson(const String &payload) {
   if (!connected_ || !mqtt.connected()) {
      connected_ = false;
      return false;
   }

   const bool ok = mqtt.publish(
      TB_TELEMETRY_TOPIC,
      payload.c_str(),
      false
   );

   if (!ok) {
      connected_ = false;
   }

   return ok;
}

bool Telemetry::publishState(const PowerState &state) {
   StaticJsonDocument<256> doc;

   doc[KEY_REDE_DISP] = state.redeDisponivel;
   doc[KEY_ALIM_REDE] = state.alimentacaoRede;
   doc[KEY_ALIM_OFFGRID] = state.alimentacaoOffgrid;
   doc[KEY_ALIM_GERADOR] = state.alimentacaoGerador;

   String payload;
   serializeJson(doc, payload);

   return publishJson(payload);
}

bool Telemetry::publishHeartbeat(
   uint32_t now,
   const PowerState &state) {

   if (static_cast<uint32_t>(now - lastHeartbeat_) < TB_HEARTBEAT_MS) {
      return false;
   }

   StaticJsonDocument<384> doc;

   doc[KEY_REDE_DISP] = state.redeDisponivel;
   doc[KEY_ALIM_REDE] = state.alimentacaoRede;
   doc[KEY_ALIM_OFFGRID] = state.alimentacaoOffgrid;
   doc[KEY_ALIM_GERADOR] = state.alimentacaoGerador;
   doc[KEY_UPTIME] = now / 1000;
   doc[KEY_WIFI_RSSI] = WiFi.RSSI();

   String payload;
   serializeJson(doc, payload);

   if (!publishJson(payload)) {
      return false;
   }

   lastHeartbeat_ = now;
   return true;
}

void Telemetry::update(uint32_t now, const PowerState &state) {
   if (WiFi.status() != WL_CONNECTED) {
      connected_ = false;
      return;
   }

   if (!mqtt.connected()) {
      connected_ = false;

      if (static_cast<uint32_t>(now - lastMqttAttempt_) >= MQTT_RECONNECT_MS) {
         lastMqttAttempt_ = now;
         connectMqtt();
      }

      return;
   }

   connected_ = true;
   mqtt.loop();

   if (!statePublished_) {
      if (publishState(state)) {
         statePublished_ = true;
      }
   }

   publishHeartbeat(now, state);
}

bool Telemetry::connected() const {
   return connected_ && mqtt.connected();
}
