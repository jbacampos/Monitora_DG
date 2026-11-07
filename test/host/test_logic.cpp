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
// ETAPA 1 visual decision: only a queued confirmed transition produces a line.
// ---------------------------------------------------------------------------------------------
void testVisualSendDecision() {
   EXPECT(visualNextSend(false) == VisualSendKind::RealChange); // queue non-empty
   EXPECT(visualNextSend(true) == VisualSendKind::None);        // queue empty
}

// ---------------------------------------------------------------------------------------------
// state.bin migration: an older image is an exact byte-prefix of the current layout (fields were
// only ever appended). The historical layouts 60/68/76/84 bytes are REAL fixtures: the byte prefix
// of a fully populated PersistedState IS the old on-flash layout. Each fixture is adopted into the
// current 120-byte layout and every field that existed before it is preserved, while the fields
// introduced later start at their safe defaults and the ETAPA-1 visual queue starts empty. An
// unknown size is rejected (never partially interpreted).
// ---------------------------------------------------------------------------------------------
void testStateMigration() {
   const size_t sizes[4] = {60, 68, 76, 84};

   EXPECT(sizeof(PersistedState) == 120);

   for (size_t s = 0; s < 4; ++s) {
      const size_t size = sizes[s];
      const int fixtureFailures = failures;

      // Populate a full current-state image; the first `size` bytes are the historical layout.
      PersistedState full{};
      full.validReliable = true;
      full.power = makeState(true, false, false, true);
      full.alive.timestamp.kind = TsKind::Epoch;
      full.alive.timestamp.value = 111;
      full.alive.redeDisponivel = true;
      full.lastFallStickerId = 222;
      full.lastReturnStickerId = 333;
      full.tgNotifiedRede = true;
      full.tgPending = true;
      full.tgForceNotify = true;
      full.tgTimestamp.kind = TsKind::Epoch;
      full.tgTimestamp.value = 444;
      full.tgPhase = TG_PHASE_TEXT_RET;
      full.tgAdjectiveIndex = 5;
      full.tgVerbIndex = 6;
      full.redeFailGraceActive = true;
      full.redeFailStarted.value = 555;
      full.tbLogHead = 0x0102;
      full.tbGapCount = 3;
      full.tgUpdateOffset = 999;
      full.pendingBootReason = BOOT_REASON_OTA;
      full.otaRequested = true;
      full.lastVisualMessageId = 777;
      full.visualDeletePendingId = 888;
      full.lastBootMessageId = 999;
      full.bootDeletePendingId = 1010;

      uint8_t fixture[84];
      memcpy(fixture, &full, size);

      EXPECT(StateMigration::isLegacySize(size));

      // Poison the whole destination to prove the adoption reinitialises it.
      PersistedState m{};
      memset(reinterpret_cast<uint8_t *>(&m), 0xAA, sizeof(m));

      EXPECT(StateMigration::adopt(fixture, size, m));

      // Fields present in EVERY historical layout (< 60 bytes): always preserved.
      EXPECT(m.validReliable);
      EXPECT_STATE(m.power, true, false, false, true);
      EXPECT(m.alive.redeDisponivel);
      EXPECT(m.alive.timestamp.value == 111);
      EXPECT(m.lastFallStickerId == 222);
      EXPECT(m.lastReturnStickerId == 333);
      EXPECT(m.tgNotifiedRede);
      EXPECT(m.tgPending);
      EXPECT(m.tgForceNotify);
      EXPECT(m.tgPhase == TG_PHASE_TEXT_RET);
      EXPECT(m.tgAdjectiveIndex == 5);
      EXPECT(m.tgVerbIndex == 6);
      EXPECT(m.redeFailGraceActive);
      EXPECT(m.tbLogHead == 0x0102);
      EXPECT(m.tbGapCount == 3);

      // tgUpdateOffset / pendingBootReason / otaRequested exist from 68 bytes on.
      if (size >= 68) {
         EXPECT(m.tgUpdateOffset == 999);
         EXPECT(m.pendingBootReason == BOOT_REASON_OTA);
         EXPECT(m.otaRequested);
      } else {
         EXPECT(m.tgUpdateOffset == 0);
         EXPECT(m.pendingBootReason == BOOT_REASON_NONE);
         EXPECT(!m.otaRequested);
      }

      // The visual-line ids exist from 76 bytes on.
      if (size >= 76) {
         EXPECT(m.lastVisualMessageId == 777);
         EXPECT(m.visualDeletePendingId == 888);
      } else {
         EXPECT(m.lastVisualMessageId == 0);
         EXPECT(m.visualDeletePendingId == 0);
      }

      // The reboot-notice ids exist from 84 bytes on.
      if (size >= 84) {
         EXPECT(m.lastBootMessageId == 999);
         EXPECT(m.bootDeletePendingId == 1010);
      } else {
         EXPECT(m.lastBootMessageId == 0);
         EXPECT(m.bootDeletePendingId == 0);
      }

      // The ETAPA-1 visual queue is always initialised empty (no artificial line).
      EXPECT(m.visualQueue.empty());
      EXPECT(m.visualQueue.count == 0);
      EXPECT(m.visualQueue.dropped == 0);

      printf("  migracao %2u B -> %u B: %s\n", static_cast<unsigned>(size), static_cast<unsigned>(sizeof(PersistedState)),
             (failures == fixtureFailures) ? "OK" : "FALHOU");
   }

   // The current size is accepted. 156 (an abandoned format that was never installed) and any
   // other size are rejected: the image is never partially interpreted.
   EXPECT(StateMigration::isKnownSize(sizeof(PersistedState)));
   EXPECT(!StateMigration::isKnownSize(156));
   EXPECT(!StateMigration::isKnownSize(85));
   EXPECT(!StateMigration::isKnownSize(sizeof(PersistedState) + 1));

   uint8_t junk[84] = {0};
   PersistedState untouched{};

   EXPECT(!StateMigration::adopt(junk, 156, untouched));
   EXPECT(!StateMigration::adopt(junk, 85, untouched));
   EXPECT(!StateMigration::adopt(junk, 60 + 1, untouched));
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
   testVisualSendDecision();
   testStateMigration();

   printf("\n%d verificacoes, %d falha(s)\n", checks, failures);

   if (failures == 0) {
      printf("OK: toda a logica pura passou.\n");
      return 0;
   }

   return (failures > 100) ? 100 : failures;
}
