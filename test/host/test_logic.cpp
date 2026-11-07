// Host tests for the pure logic of Monitora_DG (no Arduino, no network, no filesystem).
//
// Covered here:
//   - the debounce timing of one channel (unchanged behaviour);
//   - the composite gate of InputGroup, which is the fix for the impossible snapshot that
//     appeared in the "Historico de Transicoes" widget (grid available + fed by the grid +
//     fed by the offgrid at the same timestamp);
//   - the millisecond fraction of Timestamp and its conversion to/from SessionMillis;
//   - the on-flash layout guards of Timestamp/PendingRecord.
//
// Build and run (from the repository root):
//   clang++ -std=c++17 -Wall -Wextra -Iinclude test/host/test_logic.cpp -o test/host/test_logic.exe
//   .\test\host\test_logic.exe
// or with g++/MinGW:
//   g++ -std=c++17 -Wall -Wextra -Iinclude test/host/test_logic.cpp -o test/host/test_logic.exe
//
// Exit code: 0 when everything passed, otherwise the number of failed checks (capped at 100).

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "input_group.h"
#include "state_store.h"
#include "tg_maint.h"
#include "types.h"

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const char *what, int line) {
   ++checks;

   if (!condition) {
      ++failures;
      printf("FALHOU (linha %d): %s\n", line, what);
   }
}

#define EXPECT(cond) expect((cond), #cond, __LINE__)

void expectState(const PowerState &actual, bool rede, bool alimRede, bool offgrid, bool gerador, const char *what, int line) {
   ++checks;

   const bool ok = actual.redeDisponivel == rede && actual.alimentacaoRede == alimRede && actual.alimentacaoOffgrid == offgrid &&
                   actual.alimentacaoGerador == gerador;

   if (!ok) {
      ++failures;
      printf("FALHOU (linha %d): %s -> obtido rede=%d alim_rede=%d offgrid=%d gerador=%d, esperado %d%d%d%d\n", line, what,
             actual.redeDisponivel ? 1 : 0, actual.alimentacaoRede ? 1 : 0, actual.alimentacaoOffgrid ? 1 : 0, actual.alimentacaoGerador ? 1 : 0,
             rede ? 1 : 0, alimRede ? 1 : 0, offgrid ? 1 : 0, gerador ? 1 : 0);
   }
}

#define EXPECT_STATE(actual, rede, alimRede, offgrid, gerador) expectState((actual), (rede), (alimRede), (offgrid), (gerador), #actual, __LINE__)

PowerState makeState(bool rede, bool alimRede, bool offgrid, bool gerador) {
   PowerState s;
   s.redeDisponivel = rede;
   s.alimentacaoRede = alimRede;
   s.alimentacaoOffgrid = offgrid;
   s.alimentacaoGerador = gerador;
   return s;
}

// ---------------------------------------------------------------------------------------------
// On-flash layout: the millisecond fraction must live in bytes that used to be padding, so that
// state.bin / pending.bin written by 1.4.0 keep their size and offsets.
// ---------------------------------------------------------------------------------------------
void testLayout() {
   EXPECT(sizeof(Timestamp) == 8);
   EXPECT(offsetof(Timestamp, kind) == 0);
   EXPECT(offsetof(Timestamp, subsecond) == 2);
   EXPECT(offsetof(Timestamp, value) == 4);

   EXPECT(sizeof(PendingRecord) == 16);
   EXPECT(offsetof(PendingRecord, power) == 8);
   EXPECT(offsetof(PendingRecord, rebootReason) == 12);
}


// ---------------------------------------------------------------------------------------------
// One channel: the debounce timing itself is unchanged (150 ms of a stable raw level).
// ---------------------------------------------------------------------------------------------
void testDebounceTiming() {
   InputDebounce input;

   input.reset(false, 0);

   EXPECT(!input.sample(true, 100, 150)); // raw change, not confirmed
   EXPECT(!input.settled());
   EXPECT(!input.sample(true, 249, 150)); // 149 ms of stability
   EXPECT(!input.settled());
   EXPECT(input.sample(true, 250, 150)); // 150 ms -> confirms
   EXPECT(input.settled());
   EXPECT(input.stable);

   // A bounce back before the debounce completes restarts the window.
   input.reset(false, 0);
   EXPECT(!input.sample(true, 100, 150));
   EXPECT(!input.sample(false, 200, 150)); // back to the old level: nothing to confirm
   EXPECT(input.settled());
   EXPECT(!input.stable);
   EXPECT(!input.sample(true, 300, 150));
   EXPECT(!input.sample(true, 449, 150));
   EXPECT(input.sample(true, 450, 150));
   EXPECT(input.stable);
}

// ---------------------------------------------------------------------------------------------
// The composite gate: settled() is false while ANY channel is still debouncing.
// ---------------------------------------------------------------------------------------------
void testGroupSettled() {
   InputGroup group;
   const bool start[4] = {true, true, false, false};

   group.begin(start, 0, 150);

   EXPECT(group.settled());
   EXPECT_STATE(group.state(), true, true, false, false);

   bool next[4];

   for (uint8_t i = 0; i < 4; ++i) {
      next[i] = start[i];
   }

   next[3] = true;

   EXPECT(!group.sample(next, 100)); // raw change only
   EXPECT(!group.settled());         // one channel mid-debounce blocks the composite

   EXPECT(group.sample(next, 250)); // confirms the fourth channel
   EXPECT(group.settled());
   EXPECT_STATE(group.state(), true, true, false, true);
}


// ---------------------------------------------------------------------------------------------
// Regression of the widget artifact.
//
// Scenario: the ATS transfers the load back to the grid. The grid contactor closes (grid
// available = 1, fed by the grid = 1) and, 200 ms later, the offgrid contactor opens (fed by the
// offgrid = 0). A loop that samples every 100 ms and commits as soon as ANY channel confirms
// (the old InputMonitor::update + main loop) commits 1/1/1 in between, because the offgrid value
// is still the OLD stable one while the other two are already the NEW ones. That composite never
// existed: the widget showed green + blue + yellow at the same instant.
//
// With the group gate nothing is committed until the fourth channel also settles, so the
// impossible composite is never produced and the transition is reported once, complete.
// ---------------------------------------------------------------------------------------------
void testStaggeredTransferRegression() {
   InputGroup group;

   bool raw[4] = {false, false, true, false}; // steady: fed by the offgrid
   group.begin(raw, 0, 150);

   // Reference: reproduce the OLD behaviour in the test, committing a composite every time a
   // single channel confirms on its own.
   InputDebounce legacy[4];

   for (uint8_t i = 0; i < 4; ++i) {
      legacy[i].reset(raw[i], 0);
   }

   bool legacySawImpossible = false;
   bool gateSawImpossible = false;
   int commits = 0;

   for (uint32_t t = 100; t <= 2000; t += 100) {
      if (t == 1000) {
         raw[0] = true; // grid available
         raw[1] = true; // grid contactor closed
      }
      if (t == 1200) {
         raw[2] = false; // offgrid contactor opened 200 ms later
      }

      // Old behaviour: one composite per confirmed channel.
      bool legacyChanged = false;

      for (uint8_t i = 0; i < 4; ++i) {
         if (legacy[i].sample(raw[i], t, 150)) {
            legacyChanged = true;
         }
      }

      if (legacyChanged) {
         const PowerState mixed = makeState(legacy[0].stable, legacy[1].stable, legacy[2].stable, legacy[3].stable);

         if (mixed.redeDisponivel && mixed.alimentacaoRede && mixed.alimentacaoOffgrid) {
            legacySawImpossible = true;
         }
      }

      // Current behaviour (main loop): commit only a settled composite.
      group.sample(raw, t);

      if (!group.settled()) {
         continue;
      }

      const PowerState composite = group.state();

      if (composite.redeDisponivel && composite.alimentacaoRede && composite.alimentacaoOffgrid) {
         gateSawImpossible = true;
      }

      if (!samePowerState(composite, makeState(false, false, true, false))) {
         ++commits;
      }
   }

   // The old code really produced the artifact (otherwise this test would prove nothing).
   EXPECT(legacySawImpossible);

   // The gate does not.
   EXPECT(!gateSawImpossible);

   // The transfer is still reported, completely: {1,1,0} is reached.
   EXPECT(group.settled());
   EXPECT_STATE(group.state(), true, true, false, false);
   EXPECT(commits >= 1);
}


// ---------------------------------------------------------------------------------------------
// A real overlap longer than the debounce is NOT hidden: it is reported as measured (and main.cpp
// logs it). Suppressing it would hide a contactor/wiring fault, which is worse than an odd graph.
// ---------------------------------------------------------------------------------------------
void testLongOverlapIsReported() {
   InputGroup group;

   bool raw[4] = {false, false, true, false};
   group.begin(raw, 0, 150);

   bool sawImpossible = false;

   for (uint32_t t = 100; t <= 2500; t += 100) {
      if (t == 1000) {
         raw[0] = true;
         raw[1] = true;
      }
      if (t == 1600) { // 600 ms later: longer than the debounce, so the overlap is real
         raw[2] = false;
      }

      group.sample(raw, t);

      if (group.settled()) {
         const PowerState composite = group.state();

         if (composite.redeDisponivel && composite.alimentacaoRede && composite.alimentacaoOffgrid) {
            sawImpossible = true;
         }
      }
   }

   EXPECT(sawImpossible);
   EXPECT(powerStateContradictory(makeState(true, true, true, false)));
}

// ---------------------------------------------------------------------------------------------
// Timestamp: millisecond fraction, forward conversion and resolution of a SessionMillis instant.
// ---------------------------------------------------------------------------------------------
void testTimestamps() {
   const uint32_t epochAtSync = 1700000000UL;

   Timestamp ts;
   setEpochTimestampFromInstant(ts, epochAtSync, 5000, 5250);
   EXPECT(ts.kind == TsKind::Epoch);
   EXPECT(ts.value == epochAtSync);
   EXPECT(ts.subsecond == 250);
   EXPECT(timestampEpochMilliseconds(ts) == 1700000000250ULL);

   // Events inside the same second keep distinct instants (and the right order).
   Timestamp a;
   Timestamp b;
   setEpochTimestampFromInstant(a, epochAtSync, 5000, 5100);
   setEpochTimestampFromInstant(b, epochAtSync, 5000, 5900);
   EXPECT(a.value == b.value);
   EXPECT(a.subsecond != b.subsecond);
   EXPECT(a.subsecond < b.subsecond);

   // Forward conversion is exact for 5 s of millis, including the second boundaries.
   bool forwardOk = true;

   for (uint32_t elapsed = 0; elapsed <= 5000; ++elapsed) {
      Timestamp forward;
      setEpochTimestampFromInstant(forward, epochAtSync, 10000, 10000 + elapsed);

      if (timestampEpochMilliseconds(forward) != static_cast<unsigned long long>(epochAtSync) * 1000ULL + elapsed) {
         forwardOk = false;
      }
   }

   EXPECT(forwardOk);

   // Exactly at the sync instant: no elapsed time, so no fraction.
   setEpochTimestampFromInstant(ts, epochAtSync, 5000, 5000);
   EXPECT(ts.value == epochAtSync);
   EXPECT(ts.subsecond == 0);

   // Resolution of a SessionMillis instant BEFORE the sync: exact to the millisecond, and the
   // record is not resolvable twice.
   bool resolveOk = true;

   for (uint32_t elapsed = 0; elapsed <= 5000; ++elapsed) {
      Timestamp before;
      before.kind = TsKind::SessionMillis;
      before.value = 10000 - elapsed;

      Timestamp resolved = before;

      if (!resolveSessionTimestamp(resolved, epochAtSync, 10000)) {
         resolveOk = false;
         continue;
      }

      if (timestampEpochMilliseconds(resolved) != static_cast<unsigned long long>(epochAtSync) * 1000ULL - elapsed) {
         resolveOk = false;
      }

      if (resolved.kind != TsKind::Epoch) {
         resolveOk = false;
      }

      // Idempotent: an Epoch record is never shifted again.
      Timestamp again = resolved;

      if (resolveSessionTimestamp(again, epochAtSync, 10000) || again.value != resolved.value || again.subsecond != resolved.subsecond) {
         resolveOk = false;
      }
   }

   EXPECT(resolveOk);

   // Garbage in the fraction (a record written by 1.4.0) is ignored, never published.
   Timestamp garbage;
   garbage.kind = TsKind::Epoch;
   garbage.value = epochAtSync;
   garbage.subsecond = 4321;
   EXPECT(timestampEpochMilliseconds(garbage) == static_cast<unsigned long long>(epochAtSync) * 1000ULL);

   // A SessionMillis "in the future" relative to the sync is refused, not silently shifted.
   Timestamp future;
   future.kind = TsKind::SessionMillis;
   future.value = 10001;
   EXPECT(!resolveSessionTimestamp(future, epochAtSync, 10000));
   EXPECT(future.kind == TsKind::SessionMillis);
   EXPECT(future.value == 10001);

   // Only SessionMillis is resolvable.
   Timestamp already;
   already.kind = TsKind::Epoch;
   already.value = epochAtSync;
   EXPECT(!resolveSessionTimestamp(already, epochAtSync, 10000));
   EXPECT(already.value == epochAtSync);
}

// ---------------------------------------------------------------------------------------------
// Contradiction diagnostics: which combinations are flagged (never changed or dropped).
// ---------------------------------------------------------------------------------------------
void testContradictions() {
   EXPECT(!powerStateContradictory(makeState(true, true, false, false)));   // healthy: grid feeds
   EXPECT(!powerStateContradictory(makeState(false, false, true, false)));  // healthy: offgrid feeds
   EXPECT(!powerStateContradictory(makeState(false, false, false, true)));  // healthy: generator feeds
   EXPECT(!powerStateContradictory(makeState(false, false, false, false))); // transfer dead time
   EXPECT(!powerStateContradictory(makeState(true, false, false, false)));  // grid present, load elsewhere
   EXPECT(!powerStateContradictory(makeState(true, false, true, false)));   // grid present + offgrid feeds

   EXPECT(powerStateContradictory(makeState(false, true, false, false))); // fed by the grid with no grid
   EXPECT(powerStateContradictory(makeState(true, true, true, false)));   // grid + grid feed + offgrid feed
   EXPECT(powerStateContradictory(makeState(false, true, true, false)));  // grid feed + offgrid feed, no grid
}

// ---------------------------------------------------------------------------------------------
// Visual snapshot queue (ETAPA 1): a rapid FALTA -> RETORNO keeps BOTH snapshots, in order; the
// overflow coalesces the oldest and reports it (never silently).
// ---------------------------------------------------------------------------------------------
void testVisualQueue() {
   VisualQueue q;
   q.clear();
   EXPECT(q.empty());

   const PowerState fall = makeState(false, false, true, false); // FALTA: fed by the offgrid
   const PowerState back = makeState(true, true, false, false);  // RETORNO: fed by the grid

   // Rapid FALTA -> RETORNO: both preserved, in chronological order.
   EXPECT(q.push(fall));
   EXPECT(q.push(back));
   EXPECT(q.count == 2);
   EXPECT_STATE(*q.front(), false, false, true, false);
   q.pop();
   EXPECT_STATE(*q.front(), true, true, false, false);
   q.pop();
   EXPECT(q.empty());

   // Overflow: the OLDEST is coalesced and reported.
   for (uint8_t i = 0; i < VISUAL_QUEUE_CAP; ++i) {
      EXPECT(q.push(makeState((i & 1) != 0, false, false, false)));
   }
   EXPECT(q.count == VISUAL_QUEUE_CAP);
   EXPECT(!q.push(makeState(true, false, false, true))); // full -> false (logged by the caller)
   EXPECT(q.count == VISUAL_QUEUE_CAP);
   EXPECT(q.dropped == 1);
}

// ---------------------------------------------------------------------------------------------
// Real change vs silent renewal (ETAPA 2.4/2.5): a queued change always wins; the silent renewal
// only goes out when nothing is queued.
// ---------------------------------------------------------------------------------------------
void testVisualSendPriority() {
   EXPECT(visualNextSend(false, true) == VisualSendKind::RealChange);
   EXPECT(visualNextSend(false, false) == VisualSendKind::RealChange);
   EXPECT(visualNextSend(true, true) == VisualSendKind::SilentRenewal);
   EXPECT(visualNextSend(true, false) == VisualSendKind::None);
}

// ---------------------------------------------------------------------------------------------
// state.bin migration: an older image is an exact byte-prefix of the current layout. Every field
// that existed before is preserved and the appended fields stay at their safe defaults. An unknown
// size is rejected (never partially interpreted).
// ---------------------------------------------------------------------------------------------
void testStateMigration() {
   PersistedState original{};
   original.validReliable = true;
   original.power = makeState(false, false, true, false);
   original.tbLogHead = 0x01020304u;
   original.tbGapCount = 7;
   original.lastFallStickerId = 4242;
   original.lastVisualMessageId = 777;
   original.lastBootMessageId = 888;
   original.tgPhase = TG_PHASE_TEXT_RET;
   original.bootDeletePendingId = 55;

   // The first 84 bytes of the current struct ARE the 1.5.0 layout (fields were only appended), so
   // this is a faithful fixture of the previous firmware's state.bin.
   uint8_t legacy[84];
   memcpy(legacy, &original, sizeof(legacy));

   EXPECT(StateMigration::isLegacySize(60));
   EXPECT(StateMigration::isLegacySize(68));
   EXPECT(StateMigration::isLegacySize(76));
   EXPECT(StateMigration::isLegacySize(84));
   EXPECT(!StateMigration::isLegacySize(85));
   EXPECT(StateMigration::isKnownSize(sizeof(PersistedState)));
   EXPECT(!StateMigration::isKnownSize(85));
   EXPECT(!StateMigration::isKnownSize(sizeof(PersistedState) + 1));

   // Poison the appended fields to prove the migration zeroes them.
   PersistedState migrated{};
   migrated.fallStickerMaint = 9;
   migrated.visualRenewPending = 9;
   migrated.visualSentEpoch = 0xDEADBEEFu;

   EXPECT(StateMigration::adopt(legacy, 84, migrated));

   // Every field inside the first 84 bytes is preserved.
   EXPECT(migrated.validReliable);
   EXPECT_STATE(migrated.power, false, false, true, false);
   EXPECT(migrated.tbLogHead == 0x01020304u);
   EXPECT(migrated.tbGapCount == 7);
   EXPECT(migrated.lastFallStickerId == 4242);
   EXPECT(migrated.lastVisualMessageId == 777);
   EXPECT(migrated.lastBootMessageId == 888);
   EXPECT(migrated.tgPhase == TG_PHASE_TEXT_RET);
   EXPECT(migrated.bootDeletePendingId == 55);

   // The appended fields are safe defaults: no artificial deletion, empty visual queue.
   EXPECT(migrated.visualQueue.empty());
   EXPECT(migrated.fallStickerSentEpoch == 0);
   EXPECT(migrated.fallStickerNextAttempt == 0);
   EXPECT(migrated.fallStickerMaint == 0);
   EXPECT(migrated.visualSentEpoch == 0);
   EXPECT(migrated.visualRenewPending == 0);

   // An unknown size is rejected and never partially interpreted.
   PersistedState untouched{};
   EXPECT(!StateMigration::adopt(legacy, 85, untouched));
}

} // namespace

int main() {
   testLayout();
   testDebounceTiming();
   testGroupSettled();
   testStaggeredTransferRegression();
   testLongOverlapIsReported();
   testTimestamps();
   testContradictions();
   testVisualQueue();
   testVisualSendPriority();
   testStateMigration();

   printf("\n%d verificacoes, %d falha(s)\n", checks, failures);

   if (failures == 0) {
      printf("OK: toda a logica pura passou.\n");
      return 0;
   }

   return (failures > 100) ? 100 : failures;
}
