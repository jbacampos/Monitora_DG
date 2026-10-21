#include "tb_queue.h"

#include <LittleFS.h>

#include "config.h"
#include "time_source.h"

namespace {
// pending.bin is tagged with this 4-byte magic so a legacy file (12-byte records, written
// before the reboot reason existed) can be told apart from the current 16-byte format
// unambiguously and upgraded in place, instead of being misread as garbage.
//
// RDG2 (1.5.0) only redefines two bytes that used to be padding: Timestamp gained a millisecond
// fraction (subsecond) at offset 2, so sizeof(PendingRecord) and every other offset are
// unchanged. RDG1 files therefore still parse, but those two bytes hold uninitialised padding;
// begin() rewrites them once as 0 so an old record can never claim a bogus millisecond.
constexpr uint32_t PENDING_MAGIC = 0x52444732u;    // "RDG2" as stored
constexpr uint32_t PENDING_MAGIC_V1 = 0x52444731u; // "RDG1" (1.4.0 and older)
constexpr size_t PENDING_HEADER_BYTES = sizeof(PENDING_MAGIC);

enum class PendingFormat : uint8_t { Unknown, V1, Current };

uint32_t completeCount(size_t fileSize) {
   if (fileSize < PENDING_HEADER_BYTES) {
      return 0;
   }

   return static_cast<uint32_t>((fileSize - PENDING_HEADER_BYTES) / sizeof(PendingRecord));
}

// Reads the 4-byte header of an open file positioned at its start, leaving the position just
// after it. Unknown covers both a legacy file (no header at all) and a file truncated below a
// whole header.
PendingFormat readFormat(File &file) {
   uint32_t magic = 0;

   if (file.read(reinterpret_cast<uint8_t *>(&magic), sizeof(magic)) != sizeof(magic)) {
      return PendingFormat::Unknown;
   }

   if (magic == PENDING_MAGIC) {
      return PendingFormat::Current;
   }

   if (magic == PENDING_MAGIC_V1) {
      return PendingFormat::V1;
   }

   return PendingFormat::Unknown;
}

bool readMagic(File &file) {
   return readFormat(file) != PendingFormat::Unknown;
}

// A record read from a RDG1/legacy file may carry garbage in the bytes that are now `subsecond`.
void sanitizeSubsecond(PendingRecord &record) {
   if (record.timestamp.subsecond > 999) {
      record.timestamp.subsecond = 0;
   }
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

   const PendingFormat format = readFormat(file);

   if (format == PendingFormat::Unknown) {
      // Legacy layout (no header): upgrade it, preserving every complete record.
      file.close();
      return migrateLegacyFormat();
   }

   const bool needsUpgrade = (format == PendingFormat::V1);

   file.close();

   if (!needsUpgrade && ((size - PENDING_HEADER_BYTES) % sizeof(PendingRecord)) == 0) {
      return true;
   }

   if (!needsUpgrade) {
      // Drop the partial trailing record left by a power loss mid-append.
      Serial.println("pending.bin: cauda parcial reparada.");
   }

   return transformRecords(Transform::Sanitize, nullptr, 0);
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
      record.timestamp.subsecond = 0; // those bytes were padding in the legacy layout
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

   if (readBytes != sizeof(PendingRecord)) {
      return false;
   }

   sanitizeSubsecond(record);
   return true;
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

   const PendingFormat format = readFormat(src);

   if (format == PendingFormat::Unknown) {
      // Not a known format: begin() upgrades a legacy file, so there is nothing safe to
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

   // A RDG1 file always needs a rewrite (garbage subsecond bytes and the old header), even when
   // no record ends up changing.
   bool changed = (format == PendingFormat::V1);
   uint32_t index = 0;
   PendingRecord record;

   while (src.read(reinterpret_cast<uint8_t *>(&record), sizeof(PendingRecord)) == sizeof(PendingRecord)) {
      const uint32_t position = index++;

      if (record.timestamp.subsecond > 999) {
         record.timestamp.subsecond = 0;
         changed = true;
      }

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