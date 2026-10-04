#include "state_store.h"

#include <LittleFS.h>

namespace {
   uint32_t crc32(const uint8_t *data, size_t length) {
      uint32_t crc = 0xFFFFFFFF;

      for (size_t i = 0; i < length; ++i) {
         crc ^= data[i];

         for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
         }
      }

      return ~crc;
   }

   bool isValid(const PersistedState &state) {
      if (state.magic != STATE_MAGIC || state.version != STATE_VERSION) {
         return false;
      }

      PersistedState copy = state;
      const uint32_t stored = copy.crc32;
      copy.crc32 = 0;

      return stored == crc32(
         reinterpret_cast<const uint8_t *>(&copy),
         sizeof(PersistedState)
      );
   }
}

bool StateStore::begin() {
   return LittleFS.begin();
}

uint32_t StateStore::calculateCrc(const PersistedState &state) {
   PersistedState copy = state;
   copy.crc32 = 0;

   return crc32(
      reinterpret_cast<const uint8_t *>(&copy),
      sizeof(PersistedState)
   );
}

bool StateStore::readFile(const char *path, PersistedState &state) {
   File file = LittleFS.open(path, "r");

   if (!file) {
      return false;
   }

   if (file.size() != sizeof(PersistedState)) {
      file.close();
      return false;
   }

   const size_t readBytes = file.read(
      reinterpret_cast<uint8_t *>(&state),
      sizeof(PersistedState)
   );

   file.close();

   return readBytes == sizeof(PersistedState) && isValid(state);
}

bool StateStore::writeFile(const char *path, const PersistedState &state) {
   PersistedState copy = state;
   copy.crc32 = calculateCrc(copy);

   File file = LittleFS.open(path, "w");

   if (!file) {
      return false;
   }

   const size_t written = file.write(
      reinterpret_cast<const uint8_t *>(&copy),
      sizeof(PersistedState)
   );

   file.flush();
   file.close();

   return written == sizeof(PersistedState);
}

bool StateStore::load(PersistedState &state) {
   PersistedState a{};
   PersistedState b{};

   const bool validA = readFile(STATE_FILE_A, a);
   const bool validB = readFile(STATE_FILE_B, b);

   if (!validA && !validB) {
      return false;
   }

   if (validA && (!validB || a.sequence >= b.sequence)) {
      state = a;
   } else {
      state = b;
   }

   return true;
}

bool StateStore::save(const PersistedState &state) {
   PersistedState current = state;

   PersistedState a{};
   PersistedState b{};

   const bool validA = readFile(STATE_FILE_A, a);
   const bool validB = readFile(STATE_FILE_B, b);

   const uint32_t highestSequence =
      validA && validB ? max(a.sequence, b.sequence) :
      validA ? a.sequence :
      validB ? b.sequence :
      0;

   current.sequence = max(current.sequence, highestSequence + 1);
   current.crc32 = calculateCrc(current);

   // Write the less-recent slot. If power fails during the write, the other
   // valid slot remains available on the next boot.
   const bool writeA = !validA || (validB && b.sequence > a.sequence);

   return writeA
      ? writeFile(STATE_FILE_A, current)
      : writeFile(STATE_FILE_B, current);
}
