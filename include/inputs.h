#pragma once

#include "types.h"

class InputMonitor {
public:
   void begin();
   void update(uint32_t now);

   bool changed() const;
   void clearChanged();

   const PowerState &state() const;

private:
   struct Input {
      uint8_t pin;
      bool raw;
      bool stable;
      uint32_t changedAt;
   };

   Input inputs_[4]{};
   PowerState state_{};
   bool changed_ = false;

   void updateInput(Input &input, uint32_t now);
   void rebuildState();
};
