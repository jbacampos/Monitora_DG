#include "tb_queue.h"

#include <LittleFS.h>

#include "config.h"
#include "time_source.h"

namespace {
// pending.bin is tagged with this 4-byte magic so a legacy file (12-byte records, written
// before the reboot reason existed) can be told apart from the current 16-byte format
// unambiguously and upgraded in place, instead of being misread as garbage.
constexpr uint32_t PENDING_MAGIC = 0x52444731u; // "RDG1" as stored
constexpr size_t PENDING_HEADER_BYTES = sizeof(PENDING_MAGIC);

uint32_t completeCount(size_t fileSize) {
   if (fileSize < PENDING_HEADER_BYTES) {
      return 0;
   }

   return static_cast<uint32_t>((fileSize - PENDING_HEADER_BYTES) / sizeof(PendingRecord));
}

// Reads the 4-byte header of an open file positioned at its start. Returns false when the
// file does not carry the current format magic (legacy or truncated to less than a header).
// The file position is left just after the header when it matches.
bool readMagic(File &file) {
   uint32_t magic = 0;

   if (file.read(reinterpret_cast<uint8_t *>(&magic), sizeof(magic)) != sizeof(magic)) {
      return false;
   }

   return magic == PENDING_MAGIC;
}
} // namespace

uint32_t TbQueue::count() const {
   File file = LittleFS.open(PENDING_FILE, "r");

   if (!file) {
      return 0;
   }

   if (!readMagic(file)) {
      // Not the current format: never count a legacy/garbage file as valid records. begin()
      // is the place that upgrades a legacy file, so this only happens if it did not run.
      file.close();
      return 0;
   }

   const uint32_t records = completeCount(file.size());
   file.close();
   return records;
}

bool TbQueue::begin() {
   // Never leave an orphan temp file behind.
   if (LittleFS.exists(PENDING_TMP)) {
      LittleFS.remove(PENDING_TMP);
   }

   File file = LittleFS.open(PENDING_FILE, "r");

   if (!file) {
      return true;
   }

   const size_t size = file.size();

   if (size < PENDING_HEADER_BYTES) {
      // Empty or truncated below a whole header: nothing usable can be held.
      file.close();
      LittleFS.remove(PENDING_FILE);
      return true;
   }

   if (!readMagic(file)) {
      // Legacy layout (no header): upgrade it, preserving every complete record.
      file.close();
      return migrateLegacyFormat();
   }

   file.close();

   if (((size - PENDING_HEADER_BYTES) % sizeof(PendingRecord)) == 0) {
      return true;
   }

   // Drop the partial trailing record left by a power loss mid-append.
   Serial.println("pending.bin: cauda parcial reparada.");
   return transformRecords(Transform::Copy, nullptr, 0);
}

bool TbQueue::migrateLegacyFormat() {
   // The legacy layout was exactly Timestamp + PowerState = 12 bytes, with no header.
   struct LegacyRecord {
      Timestamp timestamp{};
      PowerState power{};
   };

   static_assert(sizeof(LegacyRecord) == 12, "legacy record size changed");

   File src = LittleFS.open(PENDING_FILE, "r");

   if (!src) {
      return false;
   }

   File dst = LittleFS.open(PENDING_TMP, "w");

   if (!dst) {
      src.close();
      return false;
   }

   dst.write(reinterpret_cast<const uint8_t *>(&PENDING_MAGIC), sizeof(PENDING_MAGIC));

   LegacyRecord legacy;
   PendingRecord record;

   while (src.read(reinterpret_cast<uint8_t *>(&legacy), sizeof(LegacyRecord)) == sizeof(LegacyRecord)) {
      record.timestamp = legacy.timestamp;
      record.power = legacy.power;
      record.rebootReason = REBOOT_REASON_NONE; // transitions carry no reboot reason
      dst.write(reinterpret_cast<const uint8_t *>(&record), sizeof(PendingRecord));
   }

   dst.flush();
   dst.close();
   src.close();

   Serial.println("pending.bin: formato antigo (12B) migrado para 16B; historico preservado.");

   if (!LittleFS.rename(PENDING_TMP, PENDING_FILE)) {
      LittleFS.remove(PENDING_FILE);

      if (!LittleFS.rename(PENDING_TMP, PENDING_FILE)) {
         return false;
      }
   }

   return true;
}

bool TbQueue::append(const PendingRecord &record) {
   FSInfo fsInfo;

   if (!LittleFS.info(fsInfo)) {
      return false;
   }

   if (fsInfo.usedBytes + sizeof(PendingRecord) + 4096U > fsInfo.totalBytes) {
      return false;
   }

   File file = LittleFS.open(PENDING_FILE, "a");

   if (!file) {
      return false;
   }

   const bool needsHeader = file.size() < PENDING_HEADER_BYTES;
   const size_t projected = file.size() + (needsHeader ? PENDING_HEADER_BYTES : 0) + sizeof(PendingRecord);

   if (projected > TB_LOG_BUDGET_BYTES) {
      file.close();
      return false;
   }

   // A brand-new (or truncated) file starts with the format header.
   if (needsHeader) {
      file.write(reinterpret_cast<const uint8_t *>(&PENDING_MAGIC), sizeof(PENDING_MAGIC));
   }

   const size_t written = file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(PendingRecord));
   file.flush();
   file.close();

   return written == sizeof(PendingRecord);
}

bool TbQueue::readAt(uint32_t index, PendingRecord &record) const {
   File file = LittleFS.open(PENDING_FILE, "r");

   if (!file) {
      return false;
   }

   if (!readMagic(file)) {
      file.close();
      return false;
   }

   const size_t offset = PENDING_HEADER_BYTES + static_cast<size_t>(index) * sizeof(PendingRecord);

   if (offset + sizeof(PendingRecord) > file.size()) {
      file.close();
      return false;
   }

   file.seek(offset);
   const size_t readBytes = file.read(reinterpret_cast<uint8_t *>(&record), sizeof(PendingRecord));
   file.close();

   return readBytes == sizeof(PendingRecord);
}

bool TbQueue::reset() {
   return LittleFS.remove(PENDING_FILE);
}

bool TbQueue::invalidateSessionTimestamps() {
   return transformRecords(Transform::ToLost, nullptr, 0);
}

bool TbQueue::resolveSessionTimestamps(const TimeSource &time, uint32_t head) {
   return transformRecords(Transform::Resolve, &time, head);
}

bool TbQueue::transformRecords(Transform mode, const TimeSource *time, uint32_t fromIndex) {
   File src = LittleFS.open(PENDING_FILE, "r");

   if (!src) {
      return false;
   }

   if (!readMagic(src)) {
      // Not the current format: begin() upgrades a legacy file, so there is nothing safe to
      // transform here (reading it as 16-byte records would produce garbage).
      src.close();
      return false;
   }

   File dst = LittleFS.open(PENDING_TMP, "w");

   if (!dst) {
      src.close();
      return false;
   }

   dst.write(reinterpret_cast<const uint8_t *>(&PENDING_MAGIC), sizeof(PENDING_MAGIC));

   bool changed = false;
   uint32_t index = 0;
   PendingRecord record;

   while (src.read(reinterpret_cast<uint8_t *>(&record), sizeof(PendingRecord)) == sizeof(PendingRecord)) {
      const uint32_t position = index++;

      if (mode == Transform::ToLost) {
         if (record.timestamp.kind == TsKind::SessionMillis) {
            record.timestamp.kind = TsKind::LostSession;
            changed = true;
         }
      } else if (mode == Transform::Resolve && time != nullptr && position >= fromIndex && record.timestamp.kind == TsKind::SessionMillis) {
         if (time->resolve(record.timestamp)) {
            changed = true;
         }
      }

      dst.write(reinterpret_cast<const uint8_t *>(&record), sizeof(PendingRecord));
   }

   dst.flush();
   dst.close();
   src.close();

   if (!changed) {
      LittleFS.remove(PENDING_TMP);
      return false;
   }

   if (!LittleFS.rename(PENDING_TMP, PENDING_FILE)) {
      LittleFS.remove(PENDING_FILE);

      if (!LittleFS.rename(PENDING_TMP, PENDING_FILE)) {
         return false;
      }
   }

   return true;
}