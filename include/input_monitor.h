#pragma once

#include "types.h"

// Reads the four GPIOs, applies the per-channel active level and debounces.
// It never depends on any network service.
class InputMonitor {
public:
   void begin();
   void update(uint32_t now);

   bool changed() const { return changed_; }
   void clearChanged() { changed_ = false; }

   const PowerState &state() const { return state_; }

private:
   struct Input {
      uint8_t pin;
      bool inverted;
      bool raw;
      bool stable;
      uint32_t changedAt;
   };

   Input inputs_[4]{};
   PowerState state_{};
   bool changed_ = false;

   void sample(Input &input, uint32_t now);
   void rebuild();
};