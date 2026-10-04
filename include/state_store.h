#pragma once

#include "types.h"

// Persists the single PersistedState record in LittleFS using a temp file plus
// a rename. No CRC and no A/B slots (per architecture).
class StateStore {
public:
   bool begin();
   bool load(PersistedState &state);
   bool save(const PersistedState &state);
};