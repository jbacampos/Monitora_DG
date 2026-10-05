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

   void save();
   void startSequence();
   void pickIndices(bool returnOfEnergy);
   void finishSequence(bool deliveredValue);

   void stepDelOld();
   void stepSendFall();
   void stepTextFall();
   void stepSendRet();
   void stepTextRet();

   String buildMessage(bool returnOfEnergy) const;

   int nextIndex(int *order, size_t count, size_t &position);
   void shuffle(int *order, size_t count);
};