#pragma once

// Pure policy for the visual state line (no Arduino, no network, no allocation), so it is covered
// by the host tests in test/host together with the rest of the pure logic.
//
// The queue itself lives in types.h (VisualQueue), because it is persisted inside PersistedState;
// this header only adds the *decision* of what to send next.

#include <stdint.h>

#include "types.h"

// ---------------------------------------------------------------------------------------------
// What the visual channel should send next.
//
// A real change always has priority; a silent preventive renewal is only sent when nothing else is
// queued, so a change is never delayed or replaced by a renewal. Once a real change was sent the
// caller clears visualRenewPending, so no redundant renewal follows.
// ---------------------------------------------------------------------------------------------

enum class VisualSendKind : uint8_t {
   None,         // nothing to send
   RealChange,   // a queued confirmed transition: normal notification
   SilentRenewal // the line was deleted preventively: silent replacement
};

inline VisualSendKind visualNextSend(bool queueEmpty, bool renewPending) {
   if (!queueEmpty) {
      return VisualSendKind::RealChange;
   }

   if (renewPending) {
      return VisualSendKind::SilentRenewal;
   }

   return VisualSendKind::None;
}
