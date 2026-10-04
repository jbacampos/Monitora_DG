#include "tb_queue.h"

#include <LittleFS.h>

#include "config.h"
#include "time_source.h"

namespace {
uint32_t completeCount(size_t fileSize) {
   return static_cast<uint32_t>(fileSize / sizeof(PendingRecord));
}
} // namespace

uint32_t TbQueue::count() const {
   File file = LittleFS.open(PENDING_FILE, "r");

   if (!file) {
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
   file.close();

   if ((size % sizeof(PendingRecord)) == 0) {
      return true;
   }

   // Drop the partial trailing record left by a power loss mid-append.
   Serial.println("pending.bin: cauda parcial reparada.");
   return transformRecords(Transform::Copy, nullptr, 0);
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

   if (file.size() + sizeof(PendingRecord) > TB_LOG_BUDGET_BYTES) {
      file.close();
      return false;
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

   const uint32_t offset = index * sizeof(PendingRecord);

   if (static_cast<size_t>(offset) + sizeof(PendingRecord) > file.size()) {
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

   File dst = LittleFS.open(PENDING_TMP, "w");

   if (!dst) {
      src.close();
      return false;
   }

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