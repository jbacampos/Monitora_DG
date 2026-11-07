#pragma once

// Deliberately NOT <Arduino.h>: types.h only declares plain data and pure helpers, so the
// same header compiles in the firmware and in the host tests (test/host). A .cpp that needs
// the Arduino API includes <Arduino.h> itself.
#include <stdint.h>

// The four monitored electrical signals (always logical/normalised values).
struct PowerState {
   bool redeDisponivel = false;
   bool alimentacaoRede = false;
   bool alimentacaoOffgrid = false;
   bool alimentacaoGerador = false;
};

// Explicit tag for a timestamp value, so epoch and millis() are never confused.
enum class TsKind : uint8_t {
   Epoch = 0,     // value = Unix epoch UTC (seconds); subsecond = ms within that second
   SessionMillis, // value = millis() of THIS session (resolvable on next NTP); subsecond = 0
   LostSession    // value = millis() of a PREVIOUS session (not resolvable); subsecond = 0
};

// A tagged instant.
//
// `value` keeps its original meaning (epoch seconds or millis), and `subsecond` adds the
// millisecond fraction of an Epoch timestamp (0..999). SessionMillis is already in
// milliseconds, so its subsecond is always 0.
//
// Why the fraction matters: the sampling period is 100 ms and a transfer of the ATS produces
// several transitions in a row. With whole-second epochs, every event of the same second got
// the SAME thingsboard ts, so the history could not tell them apart. ThingsBoard receives
// `ts = value * 1000 + subsecond` (see timestampEpochMilliseconds).
//
// The field occupies bytes that used to be padding, so sizeof(Timestamp) (and therefore the
// on-flash layouts of state.bin and pending.bin) is unchanged. Records written by an older
// firmware have garbage in those bytes; timestampEpochMilliseconds() treats any value above
// 999 as "no fraction" instead of publishing a bogus millisecond.
struct Timestamp {
   TsKind kind = TsKind::SessionMillis;
   uint16_t subsecond = 0;
   uint32_t value = 0;
};

static_assert(sizeof(Timestamp) == 8, "Timestamp layout changed");

// Epoch milliseconds as published to ThingsBoard. Never uses millis() as an epoch.
inline unsigned long long timestampEpochMilliseconds(const Timestamp &ts) {
   const uint32_t sub = (ts.subsecond <= 999) ? ts.subsecond : 0;
   return static_cast<unsigned long long>(ts.value) * 1000ULL + sub;
}

// Builds an Epoch timestamp from an instant of THIS session, relative to the NTP sync
// reference (epochAtSync was read at syncMillis). `now` must be >= syncMillis in the same
// millis() era.
inline void setEpochTimestampFromInstant(Timestamp &ts, uint32_t epochAtSync, uint32_t syncMillis, uint32_t instantMillis) {
   const uint32_t elapsedMs = instantMillis - syncMillis;

   ts.kind = TsKind::Epoch;
   ts.value = epochAtSync + (elapsedMs / 1000UL);
   ts.subsecond = static_cast<uint16_t>(elapsedMs % 1000UL);
}

// Resolves a SessionMillis timestamp against the NTP sync reference, in place.
//
// The reference instant is treated as epochAtSync * 1000 ms. The fraction is derived from the
// elapsed millis, so an event keeps its position INSIDE the second instead of being rounded to
// the confirmation instant. Returns false when the timestamp is not SessionMillis or claims to
// be in the future relative to the sync instant (which cannot happen for a valid session).
inline bool resolveSessionTimestamp(Timestamp &ts, uint32_t epochAtSync, uint32_t syncMillis) {
   if (ts.kind != TsKind::SessionMillis) {
      return false;
   }

   if (ts.value > syncMillis) {
      return false;
   }

   const uint32_t elapsedMs = syncMillis - ts.value;
   const uint32_t wholeSeconds = elapsedMs / 1000UL;
   const uint32_t remainderMs = elapsedMs % 1000UL;

   ts.kind = TsKind::Epoch;

   if (remainderMs == 0) {
      ts.value = epochAtSync - wholeSeconds;
      ts.subsecond = 0;
   } else {
      ts.value = epochAtSync - wholeSeconds - 1UL;
      ts.subsecond = static_cast<uint16_t>(1000UL - remainderMs);
   }

   return true;
}

struct AliveRecord {
   Timestamp timestamp{};
   bool redeDisponivel = false;
};

// Telegram recovery phases (persisted so an operation can be resumed after reboot).
enum TgPhase : uint8_t {
   TG_PHASE_NONE = 0,
   TG_PHASE_DEL_OLD,
   TG_PHASE_SEND_FALL,
   TG_PHASE_TEXT_FALL,
   TG_PHASE_SEND_RET,
   TG_PHASE_TEXT_RET
};

// Boot reason marker. The ESP8266 reports an OTA-triggered reboot as a generic software
// restart, so an update in progress is marked here before ESPhttpUpdate starts; the next
// boot then reports the OTA reason and clears the marker.
enum BootReason : uint8_t {
   BOOT_REASON_NONE = 0,
   BOOT_REASON_OTA
};

// FIFO of confirmed electrical transitions awaiting a Telegram visual line.
//
// The Telegram visual line is a "current state" mirror, but a burst of transitions (a FALTA
// followed by a RETORNO inside one text sequence, for instance) must ALL be preserved: the line
// is sent in chronological order once no FALTA/RETORNO sequence is in flight. The queue is kept
// small; on overflow the OLDEST snapshot is coalesced away and false is returned so the caller
// can log it (a pending transition is never discarded silently).
//
// Pure data plus inline helpers, so the same code runs on the target and in the host tests.
constexpr uint8_t VISUAL_QUEUE_CAP = 8;

struct VisualQueue {
   PowerState items[VISUAL_QUEUE_CAP]{};
   uint8_t head = 0;     // index of the oldest item
   uint8_t count = 0;    // number of valid items
   uint16_t dropped = 0; // diagnostic: snapshots coalesced away on overflow (never silent)

   void clear() {
      head = 0;
      count = 0;
      dropped = 0;
   }

   bool empty() const { return count == 0; }

   const PowerState *front() const { return (count == 0) ? nullptr : &items[head]; }

   // Appends one snapshot. On overflow the OLDEST snapshot is coalesced away and false is
   // returned so the caller can log it.
   bool push(const PowerState &s) {
      bool ok = true;

      if (count == VISUAL_QUEUE_CAP) {
         pop();
         ++dropped;
         ok = false;
      }

      items[(head + count) % VISUAL_QUEUE_CAP] = s;
      ++count;
      return ok;
   }

   void pop() {
      if (count == 0) {
         return;
      }

      head = static_cast<uint8_t>((head + 1) % VISUAL_QUEUE_CAP);
      --count;
   }
};

// Sticker maintenance (FALTA / RETORNO): after STICKER_DELETE_START_S the sticker is deleted and,
// once it is gone, the ORIGINAL text message is simplified with editMessageText. The association
// sticker <-> text <-> event instant <-> substitute text is kept together so it can never drift.
constexpr uint8_t STICKER_SHORT_TEXT_MAX = 44; // "🕯️Energia caiu em dd/mm/yyyy hh:mm" + NUL

struct StickerEvent {
   int32_t stickerId = 0;         // sticker message_id (0 = none / already resolved)
   int32_t textId = 0;            // associated text message_id (0 = not known yet)
   uint32_t sentEpoch = 0;        // API-confirmed send instant (Unix seconds; 0 = unknown)
   uint32_t nextAttemptEpoch = 0; // earliest next attempt (Unix seconds; 0 = no gate)
   uint8_t state = 0;             // StickerMaintState (see sticker_maint.h)
   char shortText[STICKER_SHORT_TEXT_MAX] = {0}; // substitute text preserved verbatim
};

// FIFO of sticker events whose sticker was already deleted (by the maintenance or by DEL_OLD at a
// new FALTA) but whose text still has to be simplified. Persisted so a pending edit survives a
// reboot and the association is never lost when a current slot is reused by a newer event.
constexpr uint8_t STICKER_EDIT_QUEUE_CAP = 6;

struct StickerEditQueue {
   StickerEvent items[STICKER_EDIT_QUEUE_CAP]{};
   uint8_t head = 0;     // index of the oldest item
   uint8_t count = 0;    // number of valid items
   uint16_t dropped = 0; // diagnostic: jobs coalesced away on overflow (never silent)

   void clear() {
      head = 0;
      count = 0;
      dropped = 0;
   }

   bool empty() const { return count == 0; }

   StickerEvent *front() { return (count == 0) ? nullptr : &items[head]; }

   // Appends one job. On overflow the OLDEST is coalesced away and false is returned (logged).
   bool push(const StickerEvent &e) {
      bool ok = true;

      if (count == STICKER_EDIT_QUEUE_CAP) {
         pop();
         ++dropped;
         ok = false;
      }

      items[(head + count) % STICKER_EDIT_QUEUE_CAP] = e;
      ++count;
      return ok;
   }

   void pop() {
      if (count == 0) {
         return;
      }

      head = static_cast<uint8_t>((head + 1) % STICKER_EDIT_QUEUE_CAP);
      --count;
   }
};

struct PersistedState {
   bool validReliable = false;
   PowerState power{};
   AliveRecord alive{};

   int32_t lastFallStickerId = 0;
   int32_t lastReturnStickerId = 0;

   bool tgNotifiedRede = false;
   bool tgPending = false;
   // Set by reboot reconstruction (case C). The reboot rule has priority over
   // plain coalescing, so a coincidental tgNotifiedRede must not suppress it.
   bool tgForceNotify = false;
   Timestamp tgTimestamp{};
   uint8_t tgPhase = TG_PHASE_NONE;
   uint8_t tgAdjectiveIndex = 0;
   uint8_t tgVerbIndex = 0;

   // redeDisponivel failure grace window. While active, a 1 -> 0 fall has been
   // observed but the FALTA is not confirmed yet. Persisted so a reboot does not
   // lose the window. redeFailStarted keeps the ORIGINAL fall instant, which is
   // the timestamp of the eventual FALTA event.
   bool redeFailGraceActive = false;
   Timestamp redeFailStarted{};

   uint32_t tbLogHead = 0;
   uint16_t tbGapCount = 0;

   // Last Telegram update_id + 1 that was already processed. Persisted only when a
   // received command can reboot the device before the next getUpdates(), so the same
   // /reboot or /ota is not delivered and executed again after the reset.
   int32_t tgUpdateOffset = 0;

   // Marks the next boot as caused by an OTA update instead of a generic reset.
   BootReason pendingBootReason = BOOT_REASON_NONE;

   // Set by /ota together with the restart request: the update itself runs after the next
   // boot, before Telegram and ThingsBoard exist (cleanest heap). It is consumed before the
   // attempt, so a failed download, a power cut or a watchdog reset cannot retry it
   // automatically on the following boot.
   bool otaRequested = false;

   // Visual state message (the four LEDs). Exactly one visual message is kept in
   // Telegram: lastVisualMessageId is the current one and visualDeletePendingId is a
   // previous one whose deletion Telegram has not confirmed yet (retried until it
   // succeeds). Appended at the END so the offsets of the existing fields are unchanged.
   int32_t lastVisualMessageId = 0;
   int32_t visualDeletePendingId = 0;

   // Reboot notice retention (Model B, the same scheme as the visual message). Exactly one
   // reboot notice is kept in Telegram: lastBootMessageId is the current one and
   // bootDeletePendingId is a previous one whose deletion Telegram has not confirmed yet
   // (retried until it succeeds). Appended at the END so the existing offsets are unchanged.
   int32_t lastBootMessageId = 0;
   int32_t bootDeletePendingId = 0;

   // --- Appended by firmware 1.6.0 (size-aware migration in state_store.cpp) ---
   //
   // Visual snapshot queue: confirmed electrical transitions waiting for a Telegram line. Every
   // confirmed transition is queued (captured at the transition instant) and sent in
   // chronological order once no FALTA/RETORNO sequence is in flight. Persisted so a reboot never
   // loses a confirmed transition. Appended at the END to keep every earlier field offset.
   VisualQueue visualQueue{};

   // --- Appended by firmware 1.6.1 (sticker maintenance: delete at 36 h, simplify the text) ---
   //
   // One current slot per direction (FALTA / RETORNO), independent of each other, plus a FIFO of
   // orphaned edits (a sticker already deleted by DEL_OLD while its text still owes the
   // simplification). Appended at the END to keep every earlier field offset.
   StickerEvent fallMaint{};
   StickerEvent returnMaint{};
   StickerEditQueue editQueue{};
};

// Reason of a boot/reboot. The CODE is the STABLE value published (as a NUMBER) to ThingsBoard
// and persisted in PendingRecord. The single table below maps the code to a human description
// in Portuguese, used by the Telegram boot notice, the Serial diagnostics and any future text.
// The set mirrors exactly what the firmware already reports: the persisted OTA marker plus
// every reason ESP.getResetReason() can return. REBOOT_REASON_NONE (0) means the record is an
// ordinary state transition and is never published.
//
//  1  Power On               "Energização (power-on)"
//  2  Hardware Watchdog      "Watchdog de hardware"
//  3  Exception              "Exceção"
//  4  Software Watchdog      "Watchdog de software"
//  5  Software/System reset  "Reinício por software"
//  6  Deep-Sleep Wake        "Despertar de deep sleep"
//  7  External System        "Reset externo"
//  8  Unknown                "Desconhecido"
//  9  OTA                    "Atualização (OTA)"
enum RebootReason : uint8_t {
   REBOOT_REASON_NONE = 0,
   REBOOT_REASON_POWER_ON = 1,
   REBOOT_REASON_HARDWARE_WATCHDOG = 2,
   REBOOT_REASON_EXCEPTION = 3,
   REBOOT_REASON_SOFTWARE_WATCHDOG = 4,
   REBOOT_REASON_SOFTWARE_RESTART = 5,
   REBOOT_REASON_DEEP_SLEEP = 6,
   REBOOT_REASON_EXTERNAL_SYSTEM = 7,
   REBOOT_REASON_UNKNOWN = 8,
   REBOOT_REASON_OTA = 9
};

// Single source of the human-readable reason (Portuguese). Nothing else may hard-code a
// reboot description: Telegram, Serial and any future text all use this table.
inline const char *rebootReasonDescription(uint8_t code) {
   switch (code) {
      case REBOOT_REASON_NONE:
         return "";
      case REBOOT_REASON_POWER_ON:
         return "Energização";
      case REBOOT_REASON_HARDWARE_WATCHDOG:
         return "Watchdog de hardware";
      case REBOOT_REASON_EXCEPTION:
         return "Exceção de software";
      case REBOOT_REASON_SOFTWARE_WATCHDOG:
         return "Watchdog de software";
      case REBOOT_REASON_SOFTWARE_RESTART:
         return "Reinício (/reboot)";
      case REBOOT_REASON_DEEP_SLEEP:
         return "Retorno de deep sleep";
      case REBOOT_REASON_EXTERNAL_SYSTEM:
         return "Reset externo";
      case REBOOT_REASON_OTA:
         return "Atualização (/ota)";
      case REBOOT_REASON_UNKNOWN:
      default:
         return "Desconhecido";
   }
}

// A ThingsBoard history record: a snapshot associated with a detected change.
struct PendingRecord {
   Timestamp timestamp{};
   PowerState power{};
   // 0 = ordinary snapshot (a state transition, no reboot). Otherwise the RebootReason code of
   // the mandatory boot snapshot. Appended LAST so the first 12 bytes keep the original layout:
   // an ordinary transition is published exactly as before, and a boot snapshot only adds the
   // (numeric) "reboot_reason" key to the same payload.
   uint8_t rebootReason = REBOOT_REASON_NONE;
};

// Guard against an accidental future change of the on-flash record layout.
// Expected: Timestamp 8 + PowerState 4 + rebootReason 1 (+3 padding) = 16 bytes.
static_assert(sizeof(PendingRecord) == 16, "PendingRecord size changed");

inline bool samePowerState(const PowerState &a, const PowerState &b) {
   return a.redeDisponivel == b.redeDisponivel && a.alimentacaoRede == b.alimentacaoRede && a.alimentacaoOffgrid == b.alimentacaoOffgrid &&
          a.alimentacaoGerador == b.alimentacaoGerador;
}

// True when the composite cannot be a healthy steady state of the installation:
//  - the load is fed by the grid while the grid is unavailable (backfeed), or
//  - the load is fed by the grid and by the offgrid source at the same time.
//
// This is DIAGNOSTIC ONLY. The firmware never changes, drops or postpones a state because of
// it: a contradiction that survives the debounce of all four channels is a real measurement
// (auxiliary contact skew, contactor fault or wiring fault) and must be visible. The Serial
// warning exists so such an event can be told apart from a history artifact.
inline bool powerStateContradictory(const PowerState &s) {
   return (s.alimentacaoRede && !s.redeDisponivel) || (s.alimentacaoRede && s.alimentacaoOffgrid);
}
