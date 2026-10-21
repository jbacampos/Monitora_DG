#include "time_source.h"

#include <time.h>

#include "config.h"

void TimeSource::begin() {
   configTzTime(TZ_BRASILIA, NTP_SERVER);
}

bool TimeSource::update() {
   if (synced_) {
      return false;
   }

   const time_t t = time(nullptr);

   // Before NTP, time() returns 0 (or an obviously invalid small value).
   if (t < 1600000000) {
      return false;
   }

   synced_ = true;
   epochAtSync_ = static_cast<uint32_t>(t);
   millisAtSync_ = millis();
   return true;
}

Timestamp TimeSource::now() const {
   Timestamp ts;

   if (synced_) {
      // Keep the millisecond fraction: several ATS transitions fit inside one second and each
      // must reach ThingsBoard with its own ts (see Timestamp in types.h).
      setEpochTimestampFromInstant(ts, epochAtSync_, millisAtSync_, millis());
   } else {
      ts.kind = TsKind::SessionMillis;
      ts.value = millis();
   }

   return ts;
}

bool TimeSource::resolve(Timestamp &ts) const {
   if (!synced_) {
      return false;
   }

   return resolveSessionTimestamp(ts, epochAtSync_, millisAtSync_);
}

void TimeSource::formatBrasilia(uint32_t epoch, char *buffer, size_t length) {
   const time_t raw = static_cast<time_t>(epoch);
   struct tm local {
   };

   if (localtime_r(&raw, &local) == nullptr) {
      snprintf(buffer, length, "desconhecido");
      return;
   }

   strftime(buffer, length, "%d/%m/%Y %H:%M", &local);
}