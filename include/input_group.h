#pragma once

// Pure input acquisition logic: no Arduino API, no I/O, no dynamic allocation. This is the
// part of the acquisition path that decides WHEN a composite state exists, so it is covered by
// the host tests in test/host (the pins and digitalRead() live in input_monitor.cpp).

#include <stdint.h>

#include "types.h"

// One debounced digital input. `raw` is the last observed logical level, `stable` is the level
// confirmed after `debounceMs` without change.
struct InputDebounce {
   bool raw = false;
   bool stable = false;
   uint32_t changedAt = 0;

   // Samples one level. Returns true ONLY on the sample that confirms a new stable level.
   bool sample(bool logical, uint32_t now, uint32_t debounceMs);

   // Starts from `level`, already considered stable (boot).
   void reset(bool level, uint32_t now);

   // False while a raw change is still waiting for the debounce.
   bool settled() const { return raw == stable; }
};

// The four inputs as ONE composite measurement.
//
// The four signals describe a single electrical state: the ATS transfers the load between
// mutually exclusive sources. A snapshot may only be committed when NO channel is mid-debounce.
//
// Committing on the first channel that confirms (the previous behaviour) composes the NEW value
// of that channel with the STILL OLD values of the others. During a transfer that mixture can be
// a combination that never existed together - e.g. "grid available + fed by the grid + fed by the
// offgrid" - which is exactly the impossible snapshot that appeared in the history widget.
//
// The gate changes WHEN a state is considered final, never the electrical truth: a combination
// that remains stable for the full debounce of all four channels is reported unchanged (it is a
// real measurement, possibly a fault), and the per-channel debounce/sampling timing is untouched.
class InputGroup {
public:
   static constexpr uint8_t COUNT = 4;

   void begin(const bool logical[COUNT], uint32_t now, uint32_t debounceMs);

   // Samples all channels at the same instant. Returns true when at least one stable level
   // changed (the composite may still be unsettled; check settled()).
   bool sample(const bool logical[COUNT], uint32_t now);

   // True when every channel finished debouncing: the composite is a final measurement.
   bool settled() const;

   PowerState state() const;

private:
   InputDebounce inputs_[COUNT]{};
   uint32_t debounceMs_ = 0;
};

inline void InputDebounce::reset(bool level, uint32_t now) {
   raw = level;
   stable = level;
   changedAt = now;
}

inline bool InputDebounce::sample(bool logical, uint32_t now, uint32_t debounceMs) {
   if (logical != raw) {
      raw = logical;
      changedAt = now;
      return false;
   }

   if (logical != stable && (now - changedAt) >= debounceMs) {
      stable = logical;
      return true;
   }

   return false;
}

inline void InputGroup::begin(const bool logical[COUNT], uint32_t now, uint32_t debounceMs) {
   debounceMs_ = debounceMs;

   for (uint8_t i = 0; i < COUNT; ++i) {
      inputs_[i].reset(logical[i], now);
   }
}

inline bool InputGroup::sample(const bool logical[COUNT], uint32_t now) {
   bool changed = false;

   for (uint8_t i = 0; i < COUNT; ++i) {
      if (inputs_[i].sample(logical[i], now, debounceMs_)) {
         changed = true;
      }
   }

   return changed;
}

inline bool InputGroup::settled() const {
   for (uint8_t i = 0; i < COUNT; ++i) {
      if (!inputs_[i].settled()) {
         return false;
      }
   }

   return true;
}

inline PowerState InputGroup::state() const {
   PowerState s;
   s.redeDisponivel = inputs_[0].stable;
   s.alimentacaoRede = inputs_[1].stable;
   s.alimentacaoOffgrid = inputs_[2].stable;
   s.alimentacaoGerador = inputs_[3].stable;
   return s;
}
