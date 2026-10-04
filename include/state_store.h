#pragma once

#include "types.h"

class StateStore {
public:
   bool begin();

   bool load(PersistedState &state);
   bool save(const PersistedState &state);

   static uint32_t calculateCrc(const PersistedState &state);

private:
   bool readFile(const char *path, PersistedState &state);
   bool writeFile(const char *path, const PersistedState &state);
};
