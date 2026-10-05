#include "telemetry.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "heap_diag.h"
#include "secrets.h"

namespace {
   WiFiClientSecure tlsClient;
   PubSubClient mqtt(tlsClient);
   char payloadBuffer[256];
} // namespace

bool Telemetry::begin() {
   tlsClient.setInsecure();
   tlsClient.setTimeout(1500);

   mqtt.setServer(TB_HOST, TB_PORT);
   mqtt.setBufferSize(384);

   connected_ = false;
   lastMqttAttempt_ = 0;
   lastHeartbeat_ = 0;

   return true;
}

bool Telemetry::connectMqtt() {
   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   const String clientId = String(DEVICE_ID) + "-" + String(ESP.getChipId(), HEX);

   if (mqtt.connect(clientId.c_str(), TB_ACCESS_TOKEN, nullptr)) {
      connected_ = true;
      heapDiag("evento:mqtt-conectado");
      return true;
   }

   connected_ = false;
   heapDiag("evento:mqtt-falha");
   return false;
}

bool Telemetry::publish(const char *topic, const char *payload) {
   if (!connected_ || !mqtt.connected()) {
      connected_ = false;
      return false;
   }

   const bool ok = mqtt.publish(topic, payload, false);

   if (!ok) {
      connected_ = false;
   }

   return ok;
}

bool Telemetry::publishEvent(const PendingRecord &record) {
   const PowerState &s = record.power;

   // ThingsBoard historical telemetry uses the { "ts": ..., "values": {...} } form.
   // ts is the ORIGINAL event time (never the transmission time).
   if (record.timestamp.kind == TsKind::Epoch) {
      const unsigned long long ms = static_cast<unsigned long long>(record.timestamp.value) * 1000ULL;

      snprintf(payloadBuffer, sizeof(payloadBuffer), "{\"%s\":%llu,\"values\":{\"%s\":%d,\"%s\":%d,\"%s\":%d,\"%s\":%d}}", KEY_TS, ms, KEY_REDE_DISP,
               s.redeDisponivel ? 1 : 0, KEY_ALIM_REDE, s.alimentacaoRede ? 1 : 0, KEY_ALIM_OFFGRID, s.alimentacaoOffgrid ? 1 : 0, KEY_ALIM_GERADOR,
               s.alimentacaoGerador ? 1 : 0);
   } else {
      // LostSession: the absolute time is unknown. Never fabricate an epoch.
      snprintf(payloadBuffer, sizeof(payloadBuffer), "{\"%s\":true,\"values\":{\"%s\":%d,\"%s\":%d,\"%s\":%d,\"%s\":%d}}", KEY_TS_UNKNOWN, KEY_REDE_DISP,
               s.redeDisponivel ? 1 : 0, KEY_ALIM_REDE, s.alimentacaoRede ? 1 : 0, KEY_ALIM_OFFGRID, s.alimentacaoOffgrid ? 1 : 0, KEY_ALIM_GERADOR,
               s.alimentacaoGerador ? 1 : 0);
   }

   return publish(TB_TELEMETRY_TOPIC, payloadBuffer);
}

void Telemetry::publishHeartbeat(uint32_t now, uint32_t lastAliveEpoch) {
   if ((now - lastHeartbeat_) < TB_HEARTBEAT_MS) {
      return;
   }

   lastHeartbeat_ = now;

   snprintf(payloadBuffer, sizeof(payloadBuffer), "{\"%s\":%lu,\"%s\":%lu,\"%s\":%d}", KEY_LAST_ALIVE, static_cast<unsigned long>(lastAliveEpoch), KEY_UPTIME,
            static_cast<unsigned long>(now / 1000UL), KEY_WIFI_RSSI, WiFi.RSSI());

   publish(TB_ATTRIBUTES_TOPIC, payloadBuffer);
}

void Telemetry::update(uint32_t now) {
   if (WiFi.status() != WL_CONNECTED) {
      connected_ = false;
      return;
   }

   if (!mqtt.connected()) {
      connected_ = false;

      if ((now - lastMqttAttempt_) >= MQTT_RECONNECT_MS) {
         lastMqttAttempt_ = now;

         const uint32_t tbStart = millis();

         Serial.printf("[%lu] TB: connect inicio\n", static_cast<unsigned long>(tbStart));

         const bool ok = connectMqtt();

         Serial.printf("[%lu] TB: connect fim ok=%d duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? 1 : 0,
                       static_cast<unsigned long>(millis() - tbStart));
      }

      return;
   }

   connected_ = true;
   mqtt.loop();
}