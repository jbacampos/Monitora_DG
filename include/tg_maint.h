#pragma once

// Pure policy for the two Telegram maintenance features (no Arduino, no network, no allocation),
// so it is covered by the host tests in test/host together with the rest of the pure logic.
//
//   1. The visual snapshot queue itself lives in types.h (VisualQueue), because it is persisted
//      inside PersistedState. This header only adds the *decision* of what to send next.
//   2. The preventive deletion that keeps the bot's own messages younger than Telegram's 48 h
//      limit.
//
// Everything here works with ABSOLUTE time (Unix seconds), never with millis(), so a reboot or a
// local timezone change cannot shift a deadline.

#include <stdint.h>

#include "types.h"

// ---------------------------------------------------------------------------------------------
// Preventive deletion before the Telegram 48 h limit.
//
// The Bot API refuses to delete a message that is 48 h or older. The firmware therefore starts
// trying ~60 min before that limit and keeps retrying (default 5 min) inside the last hour. It
// never schedules an attempt past the 48 h mark: if the whole window elapsed without success the
// message is reported (GiveUp) and the rest of the firmware is never blocked.
// ---------------------------------------------------------------------------------------------

constexpr uint32_t TG_MESSAGE_TTL_S   = 48UL * 60UL * 60UL;             // 172800 s
constexpr uint32_t TG_DELETE_LEAD_S   = 60UL * 60UL;                    // begin 3600 s before the limit
constexpr uint32_t TG_DELETE_RETRY_S  = 5UL * 60UL;                     // 300 s between attempts
constexpr uint32_t TG_DELETE_WINDOW_START_S = TG_MESSAGE_TTL_S - TG_DELETE_LEAD_S; // 169200 s

// Per-message maintenance state (stored as a raw uint8_t in PersistedState).
enum class MaintState : uint8_t {
   None = 0,    // nothing to track (no id, or already handled): do not act
   Waiting = 1, // a message is known and its window has not opened yet
   Done = 2,    // the deletion was confirmed (deleted / already gone / refused forever)
   Expired = 3  // the window closed without success: reported once, never retried again
};

// What to do right now for one tracked message.
enum class MaintAction : uint8_t {
   None,    // nothing to do yet
   Attempt, // attempt the deletion now
   GiveUp   // the 48 h mark passed: report and stop
};

// True once the send instant is known and the preventive window is open (>= 48 h - 60 min).
inline bool maintWindowOpen(uint32_t sentEpoch, uint32_t nowEpoch) {
   return sentEpoch != 0 && nowEpoch >= (sentEpoch + TG_DELETE_WINDOW_START_S);
}

// True once the 48 h mark passed (the deletion can no longer be guaranteed).
inline bool maintDeadlinePassed(uint32_t sentEpoch, uint32_t nowEpoch) {
   return sentEpoch != 0 && nowEpoch >= (sentEpoch + TG_MESSAGE_TTL_S);
}

// The decision for one message, using ABSOLUTE seconds. `nextAttemptEpoch` == 0 means no gate.
inline MaintAction maintDecide(uint8_t state, uint32_t sentEpoch, uint32_t nextAttemptEpoch, uint32_t nowEpoch) {
   if (state == static_cast<uint8_t>(MaintState::Done) || state == static_cast<uint8_t>(MaintState::Expired)) {
      return MaintAction::None;
   }

   if (sentEpoch == 0) {
      return MaintAction::None; // nothing usable to schedule against
   }

   if (maintDeadlinePassed(sentEpoch, nowEpoch)) {
      return MaintAction::GiveUp;
   }

   if (!maintWindowOpen(sentEpoch, nowEpoch)) {
      return MaintAction::None;
   }

   if (nextAttemptEpoch != 0 && static_cast<int32_t>(nowEpoch - nextAttemptEpoch) < 0) {
      return MaintAction::None; // still backing off (overflow-safe)
   }

   return MaintAction::Attempt;
}

// ---------------------------------------------------------------------------------------------
// What the visual channel should send next.
//
// A real change always has priority; a silent preventive renewal is only sent when nothing else
// is queued, so a change is never delayed or replaced by a renewal. Once a real change was sent
// the caller clears visualRenewPending, so no redundant renewal follows.
// ---------------------------------------------------------------------------------------------

enum class VisualSendKind : uint8_t {
   None,          // nothing to send
   RealChange,    // a queued confirmed transition: normal notification
   SilentRenewal  // the line was deleted preventively: silent replacement
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
