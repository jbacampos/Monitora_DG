#pragma once

#include "types.h"

class TimeSource;

// Append-only ThingsBoard history log stored in LittleFS (PENDING_FILE).
//
// The file starts with a 4-byte format header (PENDING_MAGIC, "RDG2") followed by PendingRecord
// records. The header lets a legacy file (12-byte records, written before the reboot history
// existed) be told apart unambiguously and upgraded in place by begin(), so history written
// by an older firmware is preserved instead of being misread. An "RDG1" header (1.4.0) is also
// recognised and upgraded once. Records are consumed strictly in order through an external head
// index kept in PersistedState (tbLogHead). The file is only truncated when the queue is fully
// drained, so history is never silently dropped.
class TbQueue {
public:
   // Repairs a partially written tail left by a power loss during an append and upgrades a
   // legacy (12-byte-record) file to the current format.
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
   // Copy: repairs the file as-is. Sanitize: same, but also zeroes the millisecond fraction of
   // records written by 1.4.0 (those bytes were padding then) and rewrites the RDG1 header.
   enum class Transform { Copy, ToLost, Resolve, Sanitize };

   bool transformRecords(Transform mode, const TimeSource *time, uint32_t fromIndex);

   // One-time upgrade of a legacy pending.bin (12-byte records, no header) to the current
   // 16-byte format, preserving every complete record (rebootReason = NONE).
   bool migrateLegacyFormat();
};