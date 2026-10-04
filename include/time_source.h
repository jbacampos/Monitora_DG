#pragma once

#include <Arduino.h>

#include "types.h"

// Owns millis() and the NTP reference clock.
//
// Before NTP is available, timestamps are tagged SessionMillis (millis() of the
// current session). Once NTP becomes available, they can be converted to Epoch.
class TimeSource {
public:
   void begin();

   // Returns true only the first time NTP becomes available in this session.
   bool update();

   bool synced() const { return synced_; }

   // Current instant as a tagged Timestamp (Epoch when NTP is available).
   Timestamp now() const;

   // Converts a SessionMillis timestamp of THIS session into Epoch (in place).
   bool resolve(Timestamp &ts) const;

   // Formats an epoch in America/Sao_Paulo as "dd/mm/yyyy HH:MM".
   static void formatBrasilia(uint32_t epoch, char *buffer, size_t length);

private:
   bool synced_ = false;
   uint32_t epochAtSync_ = 0;
   uint32_t millisAtSync_ = 0;
};