#pragma once

#include <Arduino.h>

#include "types.h"

class Telegram;
class StateStore;

// Reconciles the Telegram state. Telegram is NOT a history: it only ever has to
// reflect the current redeDisponivel state (ThingsBoard keeps the full history).
class TelegramNotifier {
public:
   static constexpr size_t kAdjectiveCount = 94;
   static constexpr size_t kFailureVerbCount = 17;
   static constexpr size_t kReturnVerbCount = 17;

   void begin(Telegram *telegram, StateStore *store, PersistedState *state);

   // A confirmed redeDisponivel transition during normal operation.
   void onRedeTransition(const Timestamp &ts);

   // A RETORNO reconstructed at boot (case C). The reboot rule has priority over
   // the plain coalescing, so this is forced.
   void onBootReturnReconstruction(const Timestamp &ts);

   // Runs at most one delivery step when Telegram is reachable.
   void update();

private:
   Telegram *telegram_ = nullptr;
   StateStore *store_ = nullptr;
   PersistedState *state_ = nullptr;

   int adjectiveOrder_[kAdjectiveCount]{};
   int failureOrder_[kFailureVerbCount]{};
   int returnOrder_[kReturnVerbCount]{};

   size_t adjectivePosition_ = kAdjectiveCount;
   size_t failurePosition_ = kFailureVerbCount;
   size_t returnPosition_ = kReturnVerbCount;

   // Diagnostic-only: millis() when the current Telegram sequence started.
   // Never persisted, never used for control flow.
   uint32_t seqStartMillis_ = 0;

   // Retry gate, RAM only (a reboot resumes the persisted phase and retries immediately).
   // It keeps a failing step from being repeated on every loop pass: nextAttemptMillis_ is the
   // earliest instant the step may run again, stepAttempts_ counts the consecutive failures of
   // the phase in stepPhase_ and retryDelayMs_ is the delay in use (reported in the log).
   uint32_t nextAttemptMillis_ = 0;
   uint16_t stepAttempts_ = 0;
   uint32_t retryDelayMs_ = 0;
   uint8_t stepPhase_ = TG_PHASE_NONE;
   bool retryPending_ = false;

   // Applies the backoff curve, honours Telegram's retry_after and abandons the sequence when
   // the attempt budget of the current step is exhausted.
   void scheduleRetry();

   void save();
   void startSequence();
   void pickIndices(bool returnOfEnergy);
   void finishSequence(bool deliveredValue);
   // Gives up on the current sequence after an error no retry can fix, leaving the state
   // consistent (phase NONE, nothing pending) so the loop and the command poll resume.
   void abortSequence(const char *reason);

   void stepDelOld();
   // Resolves one persisted sticker id: true when it no longer blocks (deleted, already gone or
   // permanently refused); false only for a transient failure. `deleted` (optional) reports
   // whether the sticker is actually GONE (deleted or already missing) as opposed to refused.
   bool resolveStickerDelete(int32_t &id, const char *label, bool *deleted = nullptr);
   void stepSendFall();
   void stepTextFall();
   void stepSendRet();
   void stepTextRet();

   // Moves a slot's still-owed text edit into the persisted edit queue (the slot is about to be
   // reused by a newer event) and clears the slot. Never loses the sticker<->text association.
   void retireStickerEvent(StickerEvent &slot, int32_t targetId, bool deleted);

   String buildMessage(bool returnOfEnergy) const;
   // Substitute text of the event currently held in state_->tgTimestamp (original instant, never
   // the edit instant): "<emoji>Energia caiu/retornou em dd/mm/yyyy hh:mm".
   String buildShortText(bool returnOfEnergy) const;

   int nextIndex(int *order, size_t count, size_t &position);
   void shuffle(int *order, size_t count);
};