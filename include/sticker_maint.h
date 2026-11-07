#pragma once

// Pure sticker-maintenance policy (no Arduino, no network, no allocation), covered by the host
// tests together with the rest of the pure logic.
//
// Lifetime of one sticker (FALTA or RETORNO), counted from the API-confirmed send instant:
//   [send, +36 h)            -> keep it
//   [+36 h, +48 h)           -> delete it, retrying every STICKER_RETRY_S (or the API retry_after)
//   >= +48 h without success -> give up (the Bot API can no longer delete it)
// The text simplification runs once the sticker is gone and is NOT bound by the 48 h limit.
//
// Everything works with ABSOLUTE time (Unix seconds), never millis(), so a reboot or a timezone
// change cannot shift a deadline.

#include <stdint.h>

#include "types.h"

constexpr uint32_t STICKER_DELETE_START_S = 36UL * 60UL * 60UL; // 129600 s = 36 h
constexpr uint32_t STICKER_TTL_S          = 48UL * 60UL * 60UL; // 172800 s = 48 h
constexpr uint32_t STICKER_RETRY_S        = 15UL * 60UL;        // 900 s = 15 min

// Per-event maintenance state (stored as a raw uint8_t in StickerEvent).
enum class StickerMaintState : uint8_t {
   None = 0,          // nothing tracked
   Waiting = 1,       // sticker sent, before the 36 h mark
   DeletePending = 2, // window open, deletion in progress
   EditPending = 3,   // sticker gone, text simplification pending
   Done = 4,          // sticker gone and text simplified (or nothing left to do)
   Expired = 5        // the 48 h window closed without a confirmed deletion
};

// What to do right now for one event.
enum class StickerAction : uint8_t {
   None,   // nothing to do
   Delete, // attempt the sticker deletion now
   Edit,   // attempt the text simplification now
   GiveUp  // the 48 h mark passed without success: report and stop deleting
};

inline StickerAction stickerDecide(StickerMaintState state, uint32_t sentEpoch, uint32_t nextAttemptEpoch, uint32_t nowEpoch) {
   // The edit is independent of the 48 h window: a deleted sticker's text is always simplified.
   if (state == StickerMaintState::EditPending) {
      if (nextAttemptEpoch != 0 && static_cast<int32_t>(nowEpoch - nextAttemptEpoch) < 0) {
         return StickerAction::None; // still backing off (overflow-safe)
      }
      return StickerAction::Edit;
   }

   if (state != StickerMaintState::Waiting && state != StickerMaintState::DeletePending) {
      return StickerAction::None;
   }

   if (sentEpoch == 0) {
      return StickerAction::None; // no usable reference instant
   }

   if (nowEpoch < (sentEpoch + STICKER_DELETE_START_S)) {
      return StickerAction::None; // before 36 h
   }

   if (nowEpoch >= (sentEpoch + STICKER_TTL_S)) {
      return StickerAction::GiveUp; // 48 h passed
   }

   if (nextAttemptEpoch != 0 && static_cast<int32_t>(nowEpoch - nextAttemptEpoch) < 0) {
      return StickerAction::None; // still backing off
   }

   return StickerAction::Delete;
}
