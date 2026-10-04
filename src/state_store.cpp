#include "state_store.h"

#include <LittleFS.h>

#include "config.h"

namespace {
bool writeReplacing(const char *tmpPath, const char *finalPath, const void *data, size_t length) {
   File file = LittleFS.open(tmpPath, "w");

   if (!file) {
      return false;
   }

   const size_t written = file.write(reinterpret_cast<const uint8_t *>(data), length);
   file.flush();
   file.close();

   if (written != length) {
      LittleFS.remove(tmpPath);
      return false;
   }

   // LittleFS rename atomically replaces an existing destination.
   if (LittleFS.rename(tmpPath, finalPath)) {
      return true;
   }

   // Defensive fallback only if overwriting is refused.
   LittleFS.remove(finalPath);
   return LittleFS.rename(tmpPath, finalPath);
}
} // namespace

bool StateStore::begin() {
   if (!LittleFS.begin()) {
      LittleFS.format();

      if (!LittleFS.begin()) {
         return false;
      }
   }

   // An orphan temp file must never prevent normal operation.
   if (LittleFS.exists(STATE_TMP)) {
      LittleFS.remove(STATE_TMP);
   }

   return true;
}

bool StateStore::load(PersistedState &state) {
   File file = LittleFS.open(STATE_FILE, "r");

   if (!file) {
      return false;
   }

   if (file.size() != sizeof(PersistedState)) {
      file.close();
      return false;
   }

   const size_t readBytes = file.read(reinterpret_cast<uint8_t *>(&state), sizeof(PersistedState));
   file.close();

   return readBytes == sizeof(PersistedState);
}

bool StateStore::save(const PersistedState &state) {
   return writeReplacing(STATE_TMP, STATE_FILE, &state, sizeof(PersistedState));
}