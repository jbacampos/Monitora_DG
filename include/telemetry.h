#pragma once

#include "types.h"

class Telemetry {
public:
   bool begin();
   void update(uint32_t now, const PowerState &state);

   bool publishState(const PowerState &state);
   bool publishHeartbeat(uint32_t now, const PowerState &state);

   bool connected() const;

private:
   uint32_t lastMqttAttempt_ = 0;
   uint32_t lastHeartbeat_ = 0;
   bool connected_ = false;
   bool statePublished_ = false;

   bool connectMqtt();
   bool publishJson(const String &payload);
};
