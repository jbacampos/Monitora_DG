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
// boot then reports the OTA reason and clears the marker.
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