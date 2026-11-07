#pragma once

#include <stddef.h>
#include <string.h>

#include "types.h"

// Persists the single PersistedState record in LittleFS using a temp file plus
// a rename. No CRC and no A/B slots (per architecture).
//
// On-flash format and migration
// -----------------------------
// Firmware 1.6.0 appended the visual snapshot queue at the END of PersistedState. Every field that
// existed before keeps its offset and type, so a state.bin written by an older firmware is an
// exact byte-prefix of the current layout. A known prefix size is adopted by copying the prefix and
// leaving the new trailing fields at their safe defaults (the visual queue starts empty). Any other
// size is treated as UNKNOWN and rejected: the image is never partially interpreted (the caller
// keeps its defaults and logs a diagnostic).
namespace StateMigration {

// Historical sizes of PersistedState, oldest first (firmware 1.0.0 .. 1.5.0). Each is a prefix of
// the current layout because fields were only ever appended.
constexpr size_t kLegacySizes[] = {60, 68, 76, 84};

// True when `fileSize` is one of the recognised historical sizes.
inline bool isLegacySize(size_t fileSize) {
   for (size_t s : kLegacySizes) {
      if (fileSize == s) {
         return true;
      }
   }

   return false;
}

// True when `fileSize` is a size this firmware can load: the current one or a recognised legacy
// prefix. Anything else must be rejected (never interpreted partially).
inline bool isKnownSize(size_t fileSize) {
   return fileSize == sizeof(PersistedState) || isLegacySize(fileSize);
}

// Adopts an on-flash image into `state`. `buffer`/`fileSize` are the bytes read from the file.
// The whole struct is zeroed first, then the prefix is copied, so the appended fields keep their
// safe defaults for a legacy image. Returns false when the size is unknown.
inline bool adopt(const uint8_t *buffer, size_t fileSize, PersistedState &state) {
   if (!isKnownSize(fileSize) || fileSize > sizeof(PersistedState)) {
      return false;
   }

   // Raw byte copy through a uint8_t* view: the on-flash image IS the layout. The cast also keeps
   // the copy explicit and avoids a (harmless) -Wclass-memaccess on the non-trivial PersistedState.
   uint8_t *dst = reinterpret_cast<uint8_t *>(&state);
   memset(dst, 0, sizeof(PersistedState));
   memcpy(dst, buffer, fileSize);
   return true;
}

} // namespace StateMigration

class StateStore {
public:
   bool begin();
   bool load(PersistedState &state);
   bool save(const PersistedState &state);
};