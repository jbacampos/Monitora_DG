#pragma once

#include "input_group.h"
#include "types.h"

// Reads the four GPIOs, applies the per-channel active level and debounces.
//
// The pins/digitalRead() part lives in the .cpp; the decision of when the four readings form a
// final composite lives in InputGroup (pure, host-testable). It never depends on any network
// service.
class InputMonitor {
public:
   void begin();
   void update(uint32_t now);

   // True when a stable level changed. Sticky: it is only cleared by clearChanged(), so an
   // edge seen while the group is still settling is not lost.
   bool changed() const { return changed_; }
   void clearChanged() { changed_ = false; }

   // Composite of the stable levels. Only a FINAL measurement while settled() is true.
   const PowerState &state() const { return state_; }

   // All four channels finished debouncing: state() can be committed as a transition.
   bool settled() const { return group_.settled(); }

private:
   struct Pin {
      uint8_t pin = 0;
      bool inverted = false;
   };

   Pin pins_[InputGroup::COUNT]{};
   InputGroup group_{};
   PowerState state_{};
   bool changed_ = false;

   void readLevels(bool levels[InputGroup::COUNT]) const;
   void rebuild();
};
