#pragma once

#include <Arduino.h>

// The four monitored electrical signals (always logical/normalised values).
struct PowerState {
   bool redeDisponivel = false;
   bool alimentacaoRede = false;
   bool alimentacaoOffgrid = false;
   bool alimentacaoGerador = false;
};

// Explicit tag for a timestamp value, so epoch and millis() are never confused.
enum class TsKind : uint8_t {
   Epoch = 0,     // value = Unix epoch UTC (seconds)
   SessionMillis, // value = millis() of THIS session (resolvable on next NTP)
   LostSession    // value = millis() of a PREVIOUS session (not resolvable)
};

struct Timestamp {
   TsKind kind = TsKind::SessionMillis;
   uint32_t value = 0;
};

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
// boot then reports "Update/OTA" and clears the marker.
enum BootReason : uint8_t {
   BOOT_REASON_NONE = 0,
   BOOT_REASON_OTA
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
};

// A ThingsBoard history record: a snapshot associated with a detected change.
struct PendingRecord {
   Timestamp timestamp{};
   PowerState power{};
};

// Guard against an accidental future change of the on-flash record layout.
// Expected: Timestamp 8 bytes + PowerState 4 bytes = 12 bytes.
static_assert(sizeof(PendingRecord) == 12, "PendingRecord size changed");

inline bool samePowerState(const PowerState &a, const PowerState &b) {
   return a.redeDisponivel == b.redeDisponivel && a.alimentacaoRede == b.alimentacaoRede && a.alimentacaoOffgrid == b.alimentacaoOffgrid &&
          a.alimentacaoGerador == b.alimentacaoGerador;
}