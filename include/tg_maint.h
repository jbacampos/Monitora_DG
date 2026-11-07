#pragma once

// Pure decision for the visual state line (no Arduino, no network, no allocation), so it is
// covered by the host tests in test/host together with the rest of the pure logic.
//
// The queue itself lives in types.h (VisualQueue), because it is persisted inside PersistedState;
// this header only answers whether a queued confirmed transition is waiting to be sent.

#include <stdint.h>

#include "types.h"

// What the visual channel should send next.
enum class VisualSendKind : uint8_t {
   None,      // nothing queued
   RealChange // a queued confirmed transition: normal notification
};

inline VisualSendKind visualNextSend(bool queueEmpty) {
   return queueEmpty ? VisualSendKind::None : VisualSendKind::RealChange;
}
