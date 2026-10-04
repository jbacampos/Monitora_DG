#pragma once

#include "types.h"

// ThingsBoard transport (MQTT over TLS).
class Telemetry {
public:
   bool begin();
   void update(uint32_t now);

   bool connected() const { return connected_; }

   // Publishes one historical event (telemetry, with its original timestamp).
   bool publishEvent(const PendingRecord &record);

   // Publishes supervision attributes. Self-throttled. lastAliveEpoch may be 0.
   void publishHeartbeat(uint32_t now, uint32_t lastAliveEpoch);

private:
   bool connectMqtt();
   bool publish(const char *topic, const char *payload);

   uint32_t lastMqttAttempt_ = 0;
   uint32_t lastHeartbeat_ = 0;
   bool connected_ = false;
};