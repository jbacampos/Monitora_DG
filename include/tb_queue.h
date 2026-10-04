#pragma once

#include "types.h"

class TimeSource;

// Append-only ThingsBoard history log stored in LittleFS (PENDING_FILE).
//
// Records are consumed strictly in order through an external head index kept in
// PersistedState (tbLogHead). The file is only truncated when the queue is fully
// drained, so history is never silently dropped.
class TbQueue {
public:
   // Repairs a partially written tail left by a power loss during an append.
   bool begin();

   uint32_t count() const;

   // Appends one record. Returns false when there is no room (storage full).
   bool append(const PendingRecord &record);

   bool readAt(uint32_t index, PendingRecord &record) const;

   // Drops the whole log. Only valid when it is fully drained.
   bool reset();

   // Converts SessionMillis records coming from a previous session into
   // LostSession. Returns true if the file was rewritten.
   bool invalidateSessionTimestamps();

   // Converts SessionMillis records (from head onwards) into Epoch using the time
   // source. Returns true if the file was rewritten.
   bool resolveSessionTimestamps(const TimeSource &time, uint32_t head);

private:
   enum class Transform { Copy, ToLost, Resolve };

   bool transformRecords(Transform mode, const TimeSource *time, uint32_t fromIndex);
};