#pragma once

#include "types.h"

class Telegram;

class Notifications {
public:
   void begin(Telegram *telegram, PersistedState *persisted);

   void onPowerStateChanged(
      const PowerState &previous,
      const PowerState &current
   );

private:
   Telegram *telegram_ = nullptr;
   PersistedState *persisted_ = nullptr;

   String makeMessage(bool returnOfEnergy);

   int nextIndex(
      int *order,
      size_t count,
      size_t &position
   );

   void shuffle(int *order, size_t count);

   static const char *adjectives_[];
   static const char *failureVerbs_[];
   static const char *returnVerbs_[];

   static constexpr size_t adjectiveCount_ = 92;
   static constexpr size_t failureVerbCount_ = 17;
   static constexpr size_t returnVerbCount_ = 17;

   int adjectiveOrder_[adjectiveCount_]{};
   int failureOrder_[failureVerbCount_]{};
   int returnOrder_[returnVerbCount_]{};

   size_t adjectivePosition_ = adjectiveCount_;
   size_t failurePosition_ = failureVerbCount_;
   size_t returnPosition_ = returnVerbCount_;
};
