#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "heap_diag.h"
#include "input_monitor.h"
#include "ota.h"
#include "secrets.h"
#include "state_store.h"
#include "status_msg.h"
#include "sticker_maint.h"
#include "tb_queue.h"
#include "telegram.h"
#include "telegram_notifier.h"
#include "telemetry.h"
#include "tg_maint.h"
#include "time_source.h"
#include "types.h"
#include "wifi_manager.h"

namespace {
   WiFiManager wifi;
   TimeSource timeSource;
   InputMonitor inputs;
   StateStore stateStore;
   TbQueue tbQueue;
   Telemetry telemetry;
   Telegram telegram;
   TelegramNotifier notifier;
   Ota ota;

   PersistedState persisted{};

   uint32_t lastSample = 0;
   uint32_t lastAlive = 0;
   uint32_t lastSavedHead = 0;
   uint32_t lastDiag = 0;

   // Diagnostic-only: edge detector so the Wi-Fi connect instant is logged once.
   bool wifiConnectedLogged = false;

   // ---------------------------------------------------------------------------
   // Visual state notice: one Telegram line with the four LEDs.
   //
   // Every CONFIRMED electrical transition is pushed onto a persisted FIFO (visualQueue) with the
   // state captured at the transition instant, and the line is sent in chronological order once no
   // FALTA/RETORNO sequence is in flight. A burst of transitions (a FALTA followed by a RETORNO)
   // is therefore never lost: the text sequence DEFERS the sends, it does not discard them. The id
   // of the current line is persisted (lastVisualMessageId / visualDeletePendingId) so a new line
   // can replace the previous one.
   // ---------------------------------------------------------------------------

   bool bootTextSent = false;
   // RAM only: the API refused the boot notice with an error no retry can fix (bad token, chat not
   // found, bot without rights). Nothing else is attempted this boot, so a dead bot cannot turn the
   // loop into a permanent request storm; any later transition re-arms it.
   bool bootNoticeAbandoned = false;
   // RAM only: the state of the last line actually sent THIS session. Used solely to avoid
   // enqueuing a duplicate when no transition happened (never to suppress a queued transition).
   bool visualStateKnown = false;
   PowerState lastVisualState{};

   // Defined after saveState(); declared here because the transition/grace paths enqueue snapshots.
   void enqueueVisual(const PowerState &s);

   constexpr char LED_REDE_ON[] = "\xF0\x9F\x9F\xA2";      // 🟢 rede disponivel
   constexpr char LED_ALIM_REDE_ON[] = "\xF0\x9F\x94\xB5"; // 🔵 alimentacao pela rede
   constexpr char LED_OFFGRID_ON[] = "\xF0\x9F\x9F\xA1";   // 🟡 alimentacao pelo offgrid
   constexpr char LED_GERADOR_ON[] = "\xF0\x9F\x94\xB4";   // 🔴 alimentacao pelo gerador
   constexpr char LED_OFF[] = "\xE2\x9A\xAA";              // ⚪ estado OFF/inativo

   // Monotonic start of the redeDisponivel failure grace window for THIS session.
   // Re-armed after a reboot because millis() does not survive a reset.
   uint32_t redeFailGraceMillis = 0;

   // Composite state captured at the INSTANT the fall was detected. The eventual FALTA event
   // carries this snapshot with redeFailStarted (the original fall instant), not the state at
   // the end of the grace window: during those 15 s the ATS normally transfers the load, so
   // using the later state back-dated "fed by the offgrid/generator" to the fall, which is how
   // a transfer could look like an impossible combination in the history.
   PowerState redeFailState{};

   void saveState() {
      if (!stateStore.save(persisted)) {
         // Diagnostic-only: a failed save would silently lose the persisted ids/state and is
         // exactly the kind of failure that breaks cross-reboot retention.
         Serial.printf("[%lu] AVISO: falha ao gravar state.bin\n", static_cast<unsigned long>(millis()));
      }
   }

   // Queues one confirmed electrical transition for the Telegram visual line. `s` is the state AT
   // the transition instant (captured by the caller), so the line reflects that instant and not the
   // state at the moment it is finally sent. Persisted, so a reboot never loses a confirmed
   // transition. The only suppression is a duplicate with the last line sent when NOTHING is
   // pending: a confirmed change that returns to a previous value is still queued.
   void enqueueVisual(const PowerState &s) {
      if (persisted.visualQueue.empty() && visualStateKnown && samePowerState(s, lastVisualState)) {
         return;
      }

      if (!persisted.visualQueue.push(s)) {
         Serial.printf("[%lu] TG: fila visual cheia; transicao mais antiga coalescida (dropped=%u)\n", static_cast<unsigned long>(millis()),
                       static_cast<unsigned>(persisted.visualQueue.dropped));
      }

      saveState();
   }

   void enqueue(const PowerState &power, const Timestamp &ts, uint8_t rebootReason = REBOOT_REASON_NONE) {
      PendingRecord record;
      record.timestamp = ts;
      record.power = power;
      record.rebootReason = rebootReason;

      if (!tbQueue.append(record)) {
         persisted.tbGapCount++;
         Serial.println("AVISO: pending.bin sem espaco; evento nao registrado (diagnostico).");
      }
   }

   // Has the redeDisponivel failure grace window elapsed?
   //
   // Preferred reference: the wall clock. When the fall instant was captured as a
   // real Epoch (NTP was available) and NTP is back, the elapsed time is measured
   // from the ORIGINAL fall, so a reboot does not restart the window: a 15 s grace
   // with the fall at 14:00:00 and a reboot at 14:00:05 still confirms at ~14:00:15.
   //
   // Conservative fallback: when there is no usable wall clock (never synced, a
   // lost session, or a clock behind the fall) the elapsed time is measured from
   // this session's re-armed monotonic timer. That can only confirm LATER than the
   // true deadline, never earlier.
   bool redeFailGraceElapsed() {
      if (persisted.redeFailStarted.kind == TsKind::Epoch && timeSource.synced()) {
         // Millisecond comparison: the fall instant carries a fraction now, and using whole
         // seconds could confirm the FALTA up to ~1 s before the window really elapsed.
         const unsigned long long startMs = timestampEpochMilliseconds(persisted.redeFailStarted);
         const unsigned long long nowMs = timestampEpochMilliseconds(timeSource.now());

         if (nowMs >= startMs) {
            return (nowMs - startMs) >= static_cast<unsigned long long>(REDE_FAIL_GRACE_MS);
         }
      }

      return static_cast<uint32_t>(millis() - redeFailGraceMillis) >= REDE_FAIL_GRACE_MS;
   }

   // Confirms a pending FALTA once the grace window has elapsed. The history event
   // and the Telegram notification keep the ORIGINAL fall instant.
   void serviceRedeFailGrace() {
      if (!persisted.redeFailGraceActive || persisted.power.redeDisponivel) {
         return;
      }

      if (!redeFailGraceElapsed()) {
         return;
      }

      persisted.redeFailGraceActive = false;

      Serial.printf("[%lu] REDE: FALTA confirmada (graca vencida) ts kind=%d value=%lu\n", static_cast<unsigned long>(millis()),
                    static_cast<int>(persisted.redeFailStarted.kind), static_cast<unsigned long>(persisted.redeFailStarted.value));

      // redeFailState is the snapshot of the fall instant, taken before the ATS transferred the
      // load; redeFailStarted is that same instant. The two always describe the same moment.
      enqueue(redeFailState, persisted.redeFailStarted);
      notifier.onRedeTransition(persisted.redeFailStarted);
      // A confirmed FALTA is a real transition: its snapshot (the state AT the fall instant) is
      // queued for the visual line, in chronological order.
      enqueueVisual(redeFailState);
      saveState();
   }

   void handleTransition(const PowerState &current, const Timestamp &ts) {
      const bool redeChanged = current.redeDisponivel != persisted.power.redeDisponivel;
      const bool previousRede = persisted.power.redeDisponivel;

      // Diagnostic only: the firmware never rewrites, drops or postpones a contradictory state.
      // A combination that survived the debounce of all four channels is a real measurement
      // (auxiliary contact skew, contactor or wiring fault); the Serial line is what tells it
      // apart from a history artifact. See powerStateContradictory() in types.h.
      if (powerStateContradictory(current)) {
         Serial.printf("[%lu] AVISO: estado eletrico contraditorio rede=%d alim_rede=%d offgrid=%d gerador=%d\n", static_cast<unsigned long>(millis()),
                       current.redeDisponivel ? 1 : 0, current.alimentacaoRede ? 1 : 0, current.alimentacaoOffgrid ? 1 : 0,
                       current.alimentacaoGerador ? 1 : 0);
      }

      // A redeDisponivel 1 -> 0 is not a FALTA yet: it opens a grace window. Until
      // it elapses there is no ThingsBoard event and no Telegram notification.
      if (redeChanged && !current.redeDisponivel) {
         if (!persisted.redeFailGraceActive) {
            persisted.redeFailGraceActive = true;
            persisted.redeFailStarted = ts;
            redeFailGraceMillis = millis();
            redeFailState = current; // state of the fall instant (the FALTA is back-dated to it)

            Serial.printf("[%lu] REDE: queda detectada (graca %lums) ts kind=%d value=%lu\n", static_cast<unsigned long>(millis()),
                          static_cast<unsigned long>(REDE_FAIL_GRACE_MS), static_cast<int>(ts.kind), static_cast<unsigned long>(ts.value));
         }

         persisted.power = current;
         saveState();
         return;
      }

      // The mains came back inside the grace window: the fall was transient.
      if (redeChanged && current.redeDisponivel && persisted.redeFailGraceActive) {
         persisted.redeFailGraceActive = false;
         redeFailState = {};

         Serial.printf("[%lu] REDE: queda transitoria descartada (graca cancelada)\n", static_cast<unsigned long>(millis()));

         persisted.power = current;
         saveState();
         return;
      }

      enqueue(current, ts);
      enqueueVisual(current); // the confirmed transition snapshot (state at this instant)
      persisted.power = current;

      if (redeChanged) {
         const uint32_t tNow = millis();

         Serial.printf("[%lu] TG: TRANSICAO rede=%d\n", static_cast<unsigned long>(tNow), current.redeDisponivel ? 1 : 0);
         Serial.printf("[%lu] TG: timestamp evento kind=%d value=%lu\n", static_cast<unsigned long>(tNow), static_cast<int>(ts.kind),
                       static_cast<unsigned long>(ts.value));
         Serial.printf("[%lu] TG: estado anterior=%d estado novo=%d\n", static_cast<unsigned long>(tNow), previousRede ? 1 : 0, current.redeDisponivel ? 1 : 0);

         notifier.onRedeTransition(ts);
      }

      saveState();
   }

   // Reason of THIS boot, captured once (see currentBootReasonCode()). Kept for the periodic
   // diagnostic line; the recorded value lives in the mandatory boot snapshot.
   uint8_t bootReasonCode = REBOOT_REASON_NONE;

   // Compact code of this boot's reason, stored in the mandatory boot snapshot. The OTA flag is
   // the very same marker the boot notice uses; every other case is classified from the exact
   // string ESP.getResetReason() returns (the same text the boot notice shows), so the published
   // "reboot_reason" matches the Telegram notice.
   uint8_t currentBootReasonCode() {
      if (persisted.pendingBootReason == BOOT_REASON_OTA) {
         return REBOOT_REASON_OTA;
      }

      // Detection is UNCHANGED: it classifies the exact string ESP.getResetReason() returns
      // (the same source the boot notice always used). Only the DISPLAY is translated to
      // Portuguese, through rebootReasonDescription().
      const String reason = ESP.getResetReason();

      if (reason == "Power On") {
         return REBOOT_REASON_POWER_ON;
      }
      if (reason == "Hardware Watchdog") {
         return REBOOT_REASON_HARDWARE_WATCHDOG;
      }
      if (reason == "Exception") {
         return REBOOT_REASON_EXCEPTION;
      }
      if (reason == "Software Watchdog") {
         return REBOOT_REASON_SOFTWARE_WATCHDOG;
      }
      if (reason == "Software/System restart") {
         return REBOOT_REASON_SOFTWARE_RESTART;
      }
      if (reason == "Deep-Sleep Wake") {
         return REBOOT_REASON_DEEP_SLEEP;
      }
      if (reason == "External System") {
         return REBOOT_REASON_EXTERNAL_SYSTEM;
      }

      return REBOOT_REASON_UNKNOWN;
   }

   void reconcileBoot() {
      bootReasonCode = currentBootReasonCode();

      const PowerState actual = inputs.state();

      if (!persisted.validReliable) {
         persisted.power = actual;
         persisted.alive.timestamp = timeSource.now();
         persisted.alive.redeDisponivel = actual.redeDisponivel;
         persisted.tgNotifiedRede = actual.redeDisponivel;
         persisted.tgPending = false;
         persisted.tgForceNotify = false;
         persisted.tgPhase = TG_PHASE_NONE;
         persisted.redeFailGraceActive = false;
         persisted.validReliable = true;

         enqueue(actual, timeSource.now(), bootReasonCode);
         saveState();
         return;
      }

      const bool before = persisted.power.redeDisponivel;
      const bool after = actual.redeDisponivel;

      // A grace window that was still pending when the device rebooted survives:
      // it is never turned into a FALTA just because of the reboot.
      if (persisted.redeFailGraceActive) {
         persisted.power = actual;

         if (after) {
            // The mains came back while the device was off: the fall was transient.
            persisted.redeFailGraceActive = false;
            redeFailState = {};
            persisted.alive.timestamp = timeSource.now();
            persisted.alive.redeDisponivel = true;

            Serial.printf("[%lu] REDE: graca descartada apos reboot (rede voltou)\n", static_cast<unsigned long>(millis()));
         } else {
            // Still down: keep waiting for the remainder of the window. The FALTA keeps the
            // snapshot of the boot that observed the fall (the only one this session has).
            redeFailState = actual;
            redeFailGraceMillis = millis();

            Serial.printf("[%lu] REDE: graca retomada apos reboot\n", static_cast<unsigned long>(millis()));
         }

         // Boot snapshot keeps its usual semantics; no FALTA is recorded here.
         enqueue(actual, timeSource.now(), bootReasonCode);
         saveState();
         return;
      }

      // A fall that is only observed after the reboot must pass the grace window
      // too, so no FALTA is confirmed here.
      if (before && !after) {
         persisted.redeFailGraceActive = true;
         persisted.redeFailStarted = persisted.alive.timestamp; // best estimate of the fall instant
         redeFailGraceMillis = millis();
         redeFailState = actual; // state observed at boot, back-dated to the estimated fall
         persisted.power = actual;

         Serial.printf("[%lu] REDE: queda detectada no boot (graca %lums)\n", static_cast<unsigned long>(millis()),
                       static_cast<unsigned long>(REDE_FAIL_GRACE_MS));

         // Boot snapshot keeps its usual semantics; the FALTA stays deferred until
         // the grace window elapses.
         enqueue(actual, timeSource.now(), bootReasonCode);
         saveState();
         return;
      }

      Timestamp reconstructed{};
      bool hasReconstruction = false;

      if (!before && after) {
         reconstructed = timeSource.now(); // CASE C
         hasReconstruction = true;
      }

      persisted.power = actual;

      if (hasReconstruction) {
         enqueue(actual, reconstructed);
         notifier.onBootReturnReconstruction(reconstructed);
      }

      if (actual.redeDisponivel) {
         persisted.alive.timestamp = timeSource.now();
         persisted.alive.redeDisponivel = true;
      }

      enqueue(actual, timeSource.now(), bootReasonCode); // mandatory boot snapshot
      saveState();
   }

   void updateAlive(uint32_t now) {
      if (!persisted.power.redeDisponivel) {
         return;
      }

      if ((now - lastAlive) < ALIVE_INTERVAL_MS) {
         return;
      }

      lastAlive = now;
      persisted.alive.timestamp = timeSource.now();
      persisted.alive.redeDisponivel = true;
      saveState();
   }

   void publishPendingTb() {
      if (!telemetry.connected()) {
         return;
      }

      const uint32_t count = tbQueue.count();

      if (persisted.tbLogHead >= count) {
         if (count > 0) {
            tbQueue.reset();
            persisted.tbLogHead = 0;
            lastSavedHead = 0;
            saveState();
         }
         return;
      }

      PendingRecord record;

      if (!tbQueue.readAt(persisted.tbLogHead, record)) {
         return;
      }

      if (record.timestamp.kind == TsKind::SessionMillis) {
         return;
      }

      const uint32_t tbStart = millis();

      Serial.printf("[%lu] TB: publishPending inicio\n", static_cast<unsigned long>(tbStart));

      const bool published = telemetry.publishEvent(record);

      Serial.printf("[%lu] TB: publishPending fim duracao=%lums\n", static_cast<unsigned long>(millis()), static_cast<unsigned long>(millis() - tbStart));

      if (!published) {
         Serial.printf("[%lu] TB: falha de publicacao\n", static_cast<unsigned long>(millis()));
         return;
      }

      Serial.printf("[%lu] TB: evento publicado\n", static_cast<unsigned long>(millis()));

      persisted.tbLogHead++;

      if (persisted.tbLogHead >= tbQueue.count() || (persisted.tbLogHead - lastSavedHead) >= TB_HEAD_SAVE_EVERY) {
         lastSavedHead = persisted.tbLogHead;
         saveState();
      }
   }

   void onTimeSynced() {
      Serial.println("NTP sincronizado.");
      heapDiag("evento:ntp-sincronizado");

      if (tbQueue.resolveSessionTimestamps(timeSource, persisted.tbLogHead)) {
         Serial.println("Log TB: timestamps de sessao convertidos para Epoch.");
      }

      bool changed = false;

      if (timeSource.resolve(persisted.alive.timestamp)) {
         changed = true;
      }

      if (timeSource.resolve(persisted.tgTimestamp)) {
         changed = true;
      }

      if (changed) {
         saveState();
      }
   }

   // RAM only: the Telegram phase being watched and the instant it was first seen, used by
   // handleTelegramCommands() to keep a wedged phase from blocking getUpdates() forever.
   uint8_t tgWatchedPhase = TG_PHASE_NONE;
   uint32_t tgWatchedSince = 0;
   bool tgStuckReported = false;
   bool tgPhaseStuck = false; // diagnostic mirror of the last evaluation (DIAG line)

   // Diagnostic-only: defined below, used by the command gate.
   const char *phaseName(uint8_t phase);

   void handleTelegramCommands(uint32_t now) {
      // While a notification sequence is in flight (tgPending with an active
      // phase), its phases must run back to back: polling getUpdates() here would
      // insert a full HTTPS round-trip between every step. Commands are still
      // processed normally as soon as the sequence has finished (phase == NONE).
      //
      // A phase that does not move for TELEGRAM_PHASE_STUCK_MS stops blocking the poll. The
      // retry/abort rules keep this from happening, and if it ever does the commands must win:
      // a wedged phase used to make /reboot and /ota unreachable while ThingsBoard kept working.
      if (persisted.tgPhase != tgWatchedPhase) {
         tgWatchedPhase = persisted.tgPhase;
         tgWatchedSince = now;
         tgStuckReported = false;
      }

      const bool activePhase = (persisted.tgPhase != TG_PHASE_NONE);
      const uint32_t phaseAge = static_cast<uint32_t>(now - tgWatchedSince);
      const bool stuck = activePhase && phaseAge >= TELEGRAM_PHASE_STUCK_MS;

      tgPhaseStuck = stuck;

      if (stuck && !tgStuckReported) {
         tgStuckReported = true;
         Serial.printf("[%lu] TG: fase %s sem progresso ha %lums; comandos liberados\n", static_cast<unsigned long>(now), phaseName(persisted.tgPhase),
                       static_cast<unsigned long>(phaseAge));
      }

      const bool notifying = persisted.tgPending && activePhase && !stuck;

      if (!notifying) {
         telegram.update(now);
      }

      if (telegram.commandOtaRequested()) {
         Serial.println("Comando /ota recebido. Verificando versao publicada.");

         // The version published on GitHub decides whether an update is requested. Nothing is
         // persisted when it cannot be verified (or when the firmware is already current), so
         // a temporary network problem can never trigger an update. When it IS newer, the
         // update itself is NOT run here: by now Telegram, ThingsBoard and the normal activity
         // have fragmented the heap, and the OTA client needs a 16 709 byte CONTIGUOUS buffer.
         // runPendingOtaIfRequested() then updates on a clean boot, before any other TLS
         // client exists. The Telegram offset is persisted first so the same /ota update is
         // not delivered again after the reset.
         String remoteVersion;
         const Ota::VersionStatus version = ota.checkVersion(remoteVersion);

         if (version == Ota::VersionStatus::Unknown) {
            Serial.println("OTA: versao remota indisponivel; atualizacao nao iniciada.");
            telegram.sendText("Não foi possível verificar a versão disponível no GitHub. Atualização não iniciada.");
         } else if (version == Ota::VersionStatus::UpToDate) {
            Serial.printf("OTA: ja atualizado (instalada=%s remota=%s)\n", FIRMWARE_VERSION, remoteVersion.c_str());
            telegram.sendText(String("Monitora_DG já está atualizado.\nVersão: ") + FIRMWARE_VERSION);
         } else {
            Serial.printf("OTA: nova versao disponivel %s (instalada %s)\n", remoteVersion.c_str(), FIRMWARE_VERSION);
            telegram.sendText(String("Nova versão disponível: ") + remoteVersion + " (instalada: " + FIRMWARE_VERSION + "). Reiniciando para atualizar.");

            persisted.tgUpdateOffset = telegram.updateOffset();
            persisted.otaRequested = true;
            saveState();

            delay(100);
            ESP.restart();
         }
      }

      if (telegram.commandRebootRequested()) {
         Serial.println("Comando /reboot recebido.");

         // Persist the Telegram offset BEFORE the reboot, otherwise the same update is
         // delivered again on the next boot and the command loops forever.
         persisted.tgUpdateOffset = telegram.updateOffset();
         saveState();

         telegram.sendText("Comando /reboot recebido. Reiniciando.");
         delay(100);
         ESP.restart();
      }

      if (telegram.commandStatusRequested()) {
         Serial.println("Comando /s recebido.");

         // Read-only diagnostic reply. The four states come from the SAME source used by the
         // monitoring and by the visual LED line (InputGroup::state() -> PowerState), the version
         // from FIRMWARE_VERSION and the uptime from millis() (monotonic). Nothing is persisted,
         // the device is not reset, no OTA runs and no external service (GitHub/ThingsBoard/NTP)
         // is needed. Posted silently, like the LED state line.
         char message[512];
         buildStatusMessage(inputs.state(), FIRMWARE_VERSION, millis(), message, sizeof(message));

         int32_t messageId = 0;

         const bool ok = telegram.sendText(String(message), messageId, true);

         Serial.printf("[%lu] TG: status /s enviado resultado=%s id=%ld\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                       static_cast<long>(messageId));
      }
   }

   // One line with the four LEDs, in the fixed order: rede disponivel, alimentacao pela
   // rede, offgrid, gerador. A dim signal is the white circle, a single ASCII space between
   // them, no text, no labels, no line break.
   String visualLine(const PowerState &s) {
      char line[24]; // 4 LEDs (3-4 bytes each) + 3 spaces + NUL

      snprintf(line, sizeof(line), "%s %s %s %s", s.redeDisponivel ? LED_REDE_ON : LED_OFF, s.alimentacaoRede ? LED_ALIM_REDE_ON : LED_OFF,
               s.alimentacaoOffgrid ? LED_OFFGRID_ON : LED_OFF, s.alimentacaoGerador ? LED_GERADOR_ON : LED_OFF);

      return String(line);
   }

   // Telegram retry gate shared by the boot notice and the LED line. A failed send must not be
   // repeated on every loop pass: each attempt is a TLS round-trip and a group chat accepts only
   // 20 messages per minute, so a burst only adds 429 (message flood) to the original problem.
   // Same curve as TelegramNotifier: 3 s doubling up to 60 s, plus Telegram's retry_after.
   bool tgRetryPending = false;
   uint32_t tgRetryNextMillis = 0;
   uint16_t tgRetryAttempts = 0;

   // True while the delay of a previous failure is still running.
   bool telegramRetryWaiting() {
      if (!tgRetryPending) {
         return false;
      }

      if (static_cast<int32_t>(millis() - tgRetryNextMillis) < 0) {
         return true;
      }

      tgRetryPending = false;
      return false;
   }

   // A step succeeded (or was abandoned): the next one runs immediately.
   void telegramRetryClear() {
      tgRetryPending = false;
      tgRetryAttempts = 0;
   }

   void telegramRetrySchedule(const char *label) {
      if (tgRetryAttempts < 0xFFFF) {
         tgRetryAttempts++;
      }

      uint16_t shifts = (tgRetryAttempts > 0) ? static_cast<uint16_t>(tgRetryAttempts - 1) : 0;

      if (shifts > 6) {
         shifts = 6;
      }

      uint32_t delayMs = TELEGRAM_RETRY_BACKOFF_MIN_MS << shifts;

      if (delayMs > TELEGRAM_RETRY_BACKOFF_MAX_MS) {
         delayMs = TELEGRAM_RETRY_BACKOFF_MAX_MS;
      }

      uint32_t retryAfterMs = static_cast<uint32_t>(telegram.lastRetryAfterSeconds()) * 1000UL;

      if (retryAfterMs > (TELEGRAM_RETRY_AFTER_MAX_S * 1000UL)) {
         retryAfterMs = TELEGRAM_RETRY_AFTER_MAX_S * 1000UL;
      }

      if (retryAfterMs > delayMs) {
         delayMs = retryAfterMs;
      }

      tgRetryPending = true;
      tgRetryNextMillis = millis() + delayMs;

      Serial.printf("[%lu] TG: retry %s em=%lums tentativa=%u\n", static_cast<unsigned long>(millis()), label,
                    static_cast<unsigned long>(delayMs), tgRetryAttempts);
   }

   // Resolves a persisted message id awaiting deletion. Returns true when the id no longer
   // blocks the caller, which must then clear it and persist: Telegram confirmed the deletion,
   // the message was already gone, or Telegram refused it permanently (message older than
   // 48 h / bot without can_delete_messages). false means a transient failure: keep the id and
   // retry later. Blocking on a permanent refusal used to wedge the whole Telegram path, since
   // the same guard also left getUpdates() (hence /reboot and /ota) unserviced.
   bool resolvePendingDelete(int32_t &pendingId, const char *label) {
      const Telegram::DeleteResult result = telegram.deleteMessage(pendingId);

      if (result == Telegram::DeleteResult::Failed) {
         return false;
      }

      if (result == Telegram::DeleteResult::Impossible) {
         Serial.printf("[%lu] TG: %s: exclusao impossivel (mais de 48h ou bot sem direito) id=%ld; id descartado\n",
                       static_cast<unsigned long>(millis()), label, static_cast<long>(pendingId));
      }

      return true;
   }

   // Sends ONE visual line per pass when Telegram is free. The line reflects a queued confirmed
   // transition (the state captured at that transition). A FALTA/RETORNO sequence in flight
   // (tgPhase != NONE) merely DEFERS the sends: the queue keeps every confirmed transition, sent
   // in chronological order once the sequence finished.
   //
   // Exactly one visual message is kept: the previous one is deleted only AFTER the new one was
   // confirmed (its id is persisted first). A pending replacement deletion is resolved first and
   // blocks the next line only on a TRANSIENT failure (a permanent refusal drops the id).
   void serviceVisualNotice() {
      // A boot notice permanently refused by the API means the bot cannot post at all: the LED line
      // is not attempted either (it would only repeat the same doomed request).
      if (bootNoticeAbandoned) {
         return;
      }

      if (!bootTextSent || WiFi.status() != WL_CONNECTED) {
         return;
      }

      if (telegramRetryWaiting()) {
         return; // a previous failure is still backing off
      }

      if (persisted.tgPhase != TG_PHASE_NONE) {
         return; // never interleave with the FALTA/RETORNO sequence
      }

      // Resolve a previous line awaiting its deletion first (Model B): the deletion is retried
      // before a new line is created.
      if (persisted.visualDeletePendingId != 0) {
         if (resolvePendingDelete(persisted.visualDeletePendingId, "LEDs antigos")) {
            persisted.visualDeletePendingId = 0;
            saveState();

            Serial.printf("[%lu] TG: LEDs antigos apagados (pendente)\n", static_cast<unsigned long>(millis()));
         } else {
            telegramRetrySchedule("LEDs antigos");
            return; // keep the id persisted and retry before sending a new line
         }
      }

      const VisualSendKind kind = visualNextSend(persisted.visualQueue.empty());

      if (kind == VisualSendKind::None) {
         return;
      }

      const PowerState line = *persisted.visualQueue.front();

      int32_t messageId = 0;

      // The LED state line is ALWAYS posted silently: it must never ring the group.
      if (telegram.sendText(visualLine(line), messageId, true) && messageId > 0) {
         telegramRetryClear();

         const int32_t previousId = persisted.lastVisualMessageId;

         // Persist the new id and the previous one BEFORE deleting: a failure between the two steps
         // can never lose the current visual state.
         persisted.lastVisualMessageId = messageId;
         persisted.visualDeletePendingId = (previousId > 0 && previousId != messageId) ? previousId : 0;

         persisted.visualQueue.pop();

         saveState();

         lastVisualState = line;
         visualStateKnown = true;

         Serial.printf("[%lu] TG: LEDs enviados rede=%d alim_rede=%d offgrid=%d gerador=%d id=%ld pendentes=%u\n",
                       static_cast<unsigned long>(millis()), line.redeDisponivel ? 1 : 0, line.alimentacaoRede ? 1 : 0,
                       line.alimentacaoOffgrid ? 1 : 0, line.alimentacaoGerador ? 1 : 0, static_cast<long>(messageId),
                       static_cast<unsigned>(persisted.visualQueue.count));

         // Replace the previous visual message. If the deletion fails, the id stays persisted and
         // the next pass retries it (blocking any new line until then).
         if (persisted.visualDeletePendingId != 0) {
            if (resolvePendingDelete(persisted.visualDeletePendingId, "LEDs antigos")) {
               persisted.visualDeletePendingId = 0;
               saveState();

               Serial.printf("[%lu] TG: LEDs antigos apagados\n", static_cast<unsigned long>(millis()));
            } else {
               Serial.printf("[%lu] TG: LEDs antigos: apagar falhou id=%ld\n", static_cast<unsigned long>(millis()),
                             static_cast<long>(persisted.visualDeletePendingId));
               telegramRetrySchedule("LEDs antigos");
            }
         }
      } else if (telegram.lastErrorFatal()) {
         // Same reasoning as the boot notice: a permanent refusal is not retried.
         bootNoticeAbandoned = true;
         Serial.printf("[%lu] TG: LEDs abandonados neste boot (error_code=%d sem recuperacao possivel)\n",
                       static_cast<unsigned long>(millis()), telegram.lastErrorCode());
      } else {
         // Transient: the queued snapshots are KEPT and retried later (never discarded here).
         telegramRetrySchedule("LEDs");
      }
   }

   // Absolute Unix time (seconds); 0 when no absolute clock exists yet (the maintenance windows
   // are absolute-time based, so it simply waits for NTP).
   uint32_t absoluteNow() {
      if (timeSource.synced()) {
         return timeSource.now().value;
      }

      return 0;
   }

   // Attempts the text simplification of one sticker event (a current slot or a queued job).
   // Returns true when it acted.
   bool stickerAttemptEdit(StickerEvent &job, uint32_t nowEpoch) {
      if (job.textId <= 0) {
         job.state = static_cast<uint8_t>(StickerMaintState::Done); // nothing to edit
         return true;
      }

      const Telegram::EditResult r = telegram.editMessageText(job.textId, String(job.shortText));

      if (r != Telegram::EditResult::Failed) {
         // Edited / already carries the substitute text / nothing to edit: done.
         job.state = static_cast<uint8_t>(StickerMaintState::Done);
         job.nextAttemptEpoch = 0;

         Serial.printf("[%lu] TG: sticker: texto simplificado id=%ld resultado=%d\n", static_cast<unsigned long>(millis()),
                       static_cast<long>(job.textId), static_cast<int>(r));
         return true;
      }

      // Transient: retry in STICKER_RETRY_S, never before the API asks.
      uint32_t wait = STICKER_RETRY_S;

      if (telegram.lastRetryAfterSeconds() > wait) {
         wait = telegram.lastRetryAfterSeconds();
      }

      job.nextAttemptEpoch = nowEpoch + wait;

      Serial.printf("[%lu] TG: sticker: edicao falhou (transitorio); nova tentativa em %us\n", static_cast<unsigned long>(millis()),
                    static_cast<unsigned>(wait));
      return true;
   }

   // One delete-or-edit step for a current FALTA/RETORNO slot. Returns true when it acted.
   bool stickerSlotStep(StickerEvent &slot, const char *label, uint32_t nowEpoch) {
      const StickerAction action = stickerDecide(static_cast<StickerMaintState>(slot.state), slot.sentEpoch, slot.nextAttemptEpoch, nowEpoch);

      if (action == StickerAction::None) {
         return false;
      }

      if (action == StickerAction::GiveUp) {
         slot.state = static_cast<uint8_t>(StickerMaintState::Expired);
         Serial.printf("[%lu] TG: %s: janela de 48h vencida sem exclusao do sticker; desistindo\n", static_cast<unsigned long>(millis()), label);
         return true;
      }

      if (action == StickerAction::Edit) {
         return stickerAttemptEdit(slot, nowEpoch);
      }

      // Delete the sticker (the 36 h window is open).
      slot.state = static_cast<uint8_t>(StickerMaintState::DeletePending);

      if (slot.stickerId <= 0) {
         // Nothing to delete: the text simplification is owed.
         slot.state = static_cast<uint8_t>(StickerMaintState::EditPending);
         slot.nextAttemptEpoch = 0;
         return true;
      }

      const Telegram::DeleteResult r = telegram.deleteMessage(slot.stickerId);

      if (r == Telegram::DeleteResult::Failed) {
         uint32_t wait = STICKER_RETRY_S;

         if (telegram.lastRetryAfterSeconds() > wait) {
            wait = telegram.lastRetryAfterSeconds();
         }

         slot.nextAttemptEpoch = nowEpoch + wait;

         Serial.printf("[%lu] TG: %s: exclusao falhou (transitorio); nova tentativa em %us\n", static_cast<unsigned long>(millis()), label,
                       static_cast<unsigned>(wait));
         return true;
      }

      if (r == Telegram::DeleteResult::Impossible) {
         // Telegram will never delete it: the sticker stays and its text is NOT simplified.
         slot.state = static_cast<uint8_t>(StickerMaintState::Expired);
         slot.stickerId = 0;
         slot.nextAttemptEpoch = 0;

         Serial.printf("[%lu] TG: %s: exclusao impossivel; sticker permanece (sem simplificacao do texto)\n",
                       static_cast<unsigned long>(millis()), label);
         return true;
      }

      // Deleted / NotFound: the sticker is gone and the text simplification is owed.
      slot.state = static_cast<uint8_t>(StickerMaintState::EditPending);
      slot.stickerId = 0;
      slot.nextAttemptEpoch = 0;

      Serial.printf("[%lu] TG: %s: sticker resolvido; edicao do texto pendente\n", static_cast<unsigned long>(millis()), label);
      return true;
   }

   // Sticker maintenance: deletes the FALTA/RETORNO stickers at 36 h and simplifies the associated
   // texts once a sticker is gone. Runs at most one action per pass, only when no FALTA/RETORNO
   // sequence is in flight and Telegram is free, so it never blocks the FIFO line, ThingsBoard,
   // /reboot, /ota or new transitions.
   void serviceStickerMaint() {
      if (WiFi.status() != WL_CONNECTED) {
         return;
      }

      if (persisted.tgPhase != TG_PHASE_NONE) {
         return;
      }

      if (telegramRetryWaiting()) {
         return; // a send is backing off; do not add requests
      }

      const uint32_t nowEpoch = absoluteNow();

      if (nowEpoch == 0) {
         return; // the windows need absolute time
      }

      bool acted = false;

      if (stickerSlotStep(persisted.fallMaint, "FALTA", nowEpoch)) {
         acted = true;
      } else if (stickerSlotStep(persisted.returnMaint, "RETORNO", nowEpoch)) {
         acted = true;
      } else {
         StickerEvent *job = persisted.editQueue.front();

         if (job != nullptr && stickerAttemptEdit(*job, nowEpoch)) {
            if (job->state == static_cast<uint8_t>(StickerMaintState::Done)) {
               persisted.editQueue.pop();
            }

            acted = true;
         }
      }

      if (acted) {
         saveState();
      }
   }

   // One Telegram reboot notice per boot. Exactly one notice is kept, using the same Model B
   // as the visual message: the new message_id and the previous one are persisted BEFORE the
   // previous is deleted, and while a deletion is pending no new notice is created, so orphan
   // bubbles never accumulate. The content of the message is unchanged.
   // The bootTextSent flag lives in RAM only, so every reset re-arms it; the loop is the retry
   // mechanism and nothing here blocks the boot (setup() never waits for Telegram). An OTA
   // reboot is reported by the ESP8266 as a generic software restart, so it is identified by
   // the marker persisted before the update started (pendingBootReason).
   void serviceBootNotice() {
      if (WiFi.status() != WL_CONNECTED) {
         return;
      }

      if (telegramRetryWaiting()) {
         return; // a previous failure is still backing off
      }

      // A previous notice awaiting its deletion is resolved FIRST - on every pass, even after
      // this boot's notice was sent. While it is pending no new notice is created (a new one
      // would only pile up more pending deletions).
      if (persisted.bootDeletePendingId != 0) {
         if (resolvePendingDelete(persisted.bootDeletePendingId, "notificacao de boot anterior")) {
            persisted.bootDeletePendingId = 0;
            saveState();

            Serial.printf("[%lu] TG: notificacao de boot anterior apagada (pendente)\n", static_cast<unsigned long>(millis()));
         } else {
            telegramRetrySchedule("notificacao de boot anterior");
            return; // keep the id persisted and retry; no new notice while pending
         }
      }

      if (bootTextSent) {
         return;
      }

      const bool fromUpdate = (persisted.pendingBootReason == BOOT_REASON_OTA);
      const String reason = rebootReasonDescription(bootReasonCode);
      const String message = String("Monitora_DG reiniciado.\nVersão: ") + FIRMWARE_VERSION + "\nMotivo: " + reason;

      int32_t messageId = 0;

      if (telegram.sendText(message, messageId) && messageId > 0) {
         telegramRetryClear();

         const int32_t previousId = persisted.lastBootMessageId;

         // Persist the new id (and the previous one as pending) BEFORE deleting, so a failure
         // between the two steps can never lose the current reboot notice.
         persisted.lastBootMessageId = messageId;
         persisted.bootDeletePendingId = (previousId > 0 && previousId != messageId) ? previousId : 0;

         // Consume the OTA marker only after the message was actually delivered, together with
         // the ids, in a single save.
         if (fromUpdate) {
            persisted.pendingBootReason = BOOT_REASON_NONE;
         }

         saveState();

         bootTextSent = true;
         // The boot line follows the boot text (serviceVisualNotice(), same loop pass). It is a
         // normal queued snapshot of the current state; at boot visualStateKnown is false, so it is
         // always enqueued.
         enqueueVisual(inputs.state());

         Serial.printf("[%lu] TG: notificacao de boot enviada (motivo=%s) id=%ld\n", static_cast<unsigned long>(millis()),
                       rebootReasonDescription(bootReasonCode), static_cast<long>(messageId));

         // Replace the previous reboot notice. If the deletion fails, its id stays persisted
         // and the next pass retries it (blocking any new notice until then).
         if (persisted.bootDeletePendingId != 0) {
            if (resolvePendingDelete(persisted.bootDeletePendingId, "notificacao de boot anterior")) {
               persisted.bootDeletePendingId = 0;
               saveState();

               Serial.printf("[%lu] TG: notificacao de boot anterior apagada\n", static_cast<unsigned long>(millis()));
            } else {
               Serial.printf("[%lu] TG: notificacao de boot anterior: apagar falhou id=%ld\n", static_cast<unsigned long>(millis()),
                             static_cast<long>(persisted.bootDeletePendingId));
               telegramRetrySchedule("notificacao de boot anterior");
            }
         }
      } else if (telegram.lastErrorFatal()) {
         // Nothing can be delivered this boot: no token/chat/rights. Not retried until the next
         // transition, so the loop is not turned into a request storm.
         bootNoticeAbandoned = true;
         Serial.printf("[%lu] TG: aviso de boot abandonado neste boot (error_code=%d sem recuperacao possivel)\n",
                       static_cast<unsigned long>(millis()), telegram.lastErrorCode());
      } else {
         telegramRetrySchedule("aviso de boot");
      }
   }

   // Runs a pending /ota update on a clean boot. Called from setup() right after wifi.begin()
   // and BEFORE telegram.begin()/telemetry.begin(), so the OTA client is the first TLS client
   // of the boot and gets the least fragmented heap. Ota::run() is used exactly as it is.
   void runPendingOtaIfRequested() {
      if (!persisted.otaRequested) {
         return;
      }

      const uint32_t waitStart = millis();

      while (!wifi.connected() && (millis() - waitStart) < OTA_BOOT_WIFI_WAIT_MS) {
         wifi.update(millis());
         delay(50);
      }

      // Consume the request BEFORE attempting the update: a failed download, a power cut or
      // a watchdog reset then cannot trigger another automatic attempt on the next boot.
      persisted.otaRequested = false;
      // If the update completes, ESPhttpUpdate reboots the device and the next boot reports
      // this as the reset reason.
      persisted.pendingBootReason = BOOT_REASON_OTA;
      saveState();

      if (!wifi.connected()) {
         persisted.pendingBootReason = BOOT_REASON_NONE;
         saveState();
         Serial.printf("[%lu] OTA: sem Wi-Fi em %lums - atualizacao nao iniciada\n", static_cast<unsigned long>(millis()),
                       static_cast<unsigned long>(OTA_BOOT_WIFI_WAIT_MS));
         return;
      }

      Serial.printf("[%lu] OTA: solicitacao pendente, executando no boot limpo\n", static_cast<unsigned long>(millis()));

      ota.run();

      // Only reached when the update did NOT reboot the device, i.e. it failed: report it
      // and let the normal boot continue, with no marker that could claim a successful OTA.
      persisted.pendingBootReason = BOOT_REASON_NONE;
      saveState();

      Serial.printf("[%lu] OTA: tentativa falhou - seguindo o boot normal\n", static_cast<unsigned long>(millis()));
   }

   const char *phaseName(uint8_t phase) {
      switch (phase) {
         case TG_PHASE_NONE:
            return "none";
         case TG_PHASE_DEL_OLD:
            return "del_old";
         case TG_PHASE_SEND_FALL:
            return "send_fall";
         case TG_PHASE_TEXT_FALL:
            return "text_fall";
         case TG_PHASE_SEND_RET:
            return "send_ret";
         case TG_PHASE_TEXT_RET:
            return "text_ret";
         default:
            return "?";
      }
   }

   void printState(const char *prefix, const PowerState &s) {
      Serial.printf("%s rede=%d alim_rede=%d offgrid=%d gerador=%d\n", prefix, s.redeDisponivel, s.alimentacaoRede, s.alimentacaoOffgrid, s.alimentacaoGerador);
   }

   void printDiagnostics(uint32_t now) {
      if ((now - lastDiag) < DIAGNOSTIC_INTERVAL_MS) {
         return;
      }

      lastDiag = now;

      const uint32_t count = tbQueue.count();

      // Counters printed next to the existing DIAG line.
      Serial.printf("[%lu] TG DIAG: reuse_attempts=%lu reuse_success=%lu reuse_stalls=%lu new_connections=%lu\n", static_cast<unsigned long>(now),
                    static_cast<unsigned long>(telegram.reuseAttempts()), static_cast<unsigned long>(telegram.reuseSuccess()),
                    static_cast<unsigned long>(telegram.reuseStalls()), static_cast<unsigned long>(telegram.newConnections()));

      Serial.printf("DIAG up=%lus wifi=%d rssi=%d ntp=%d rede=%d pend=%lu head=%lu gap=%u tgPend=%d tgNotif=%d tgForce=%d tgPhase=%s tgStuck=%d "
                    "fallId=%ld retId=%ld visId=%ld visDelId=%ld vq=%u vqDrop=%u fallM=%d retM=%d eq=%u eqDrop=%u\n",
                    static_cast<unsigned long>(now / 1000UL), WiFi.status() == WL_CONNECTED ? 1 : 0, WiFi.RSSI(), timeSource.synced() ? 1 : 0,
                    persisted.power.redeDisponivel ? 1 : 0, static_cast<unsigned long>(count - persisted.tbLogHead),
                    static_cast<unsigned long>(persisted.tbLogHead), persisted.tbGapCount, persisted.tgPending ? 1 : 0, persisted.tgNotifiedRede ? 1 : 0,
                    persisted.tgForceNotify ? 1 : 0, phaseName(persisted.tgPhase), tgPhaseStuck ? 1 : 0, static_cast<long>(persisted.lastFallStickerId),
                    static_cast<long>(persisted.lastReturnStickerId), static_cast<long>(persisted.lastVisualMessageId),
                    static_cast<long>(persisted.visualDeletePendingId), static_cast<unsigned>(persisted.visualQueue.count),
                    static_cast<unsigned>(persisted.visualQueue.dropped), persisted.fallMaint.state, persisted.returnMaint.state,
                    static_cast<unsigned>(persisted.editQueue.count), static_cast<unsigned>(persisted.editQueue.dropped));

      Serial.printf("[%lu] BOOT DIAG: bootId=%ld bootDelId=%ld reason=%s\n", static_cast<unsigned long>(now),
                    static_cast<long>(persisted.lastBootMessageId), static_cast<long>(persisted.bootDeletePendingId),
                    rebootReasonDescription(bootReasonCode));
   }
} // namespace

void setup() {
   Serial.begin(115200);
   delay(50);

   Serial.println();
   Serial.println("Boot Monitora_DG");

   heapDiag("setup:boot");

   if (!stateStore.begin()) {
      Serial.println("ERRO: LittleFS");
   }

   heapDiag("setup:pos-littlefs");

   tbQueue.begin();
   tbQueue.invalidateSessionTimestamps();

   heapDiag("setup:pos-tbqueue");

   const bool haveState = stateStore.load(persisted);

   inputs.begin();

   heapDiag("setup:pos-gpio");

   timeSource.begin();

   heapDiag("setup:pos-ntp-start");

   if (haveState) {
      const uint32_t count = tbQueue.count();

      if (persisted.tbLogHead > count) {
         persisted.tbLogHead = count;
      }

      if (persisted.alive.timestamp.kind == TsKind::SessionMillis) {
         persisted.alive.timestamp.kind = TsKind::LostSession;
      }

      if (persisted.tgTimestamp.kind == TsKind::SessionMillis) {
         persisted.tgTimestamp.kind = TsKind::LostSession;
      }

      if (persisted.redeFailStarted.kind == TsKind::SessionMillis) {
         persisted.redeFailStarted.kind = TsKind::LostSession;
      }

      Serial.println("Estado persistente carregado.");
   } else {
      Serial.println("Sem estado persistente (primeira inicializacao).");
   }

   heapDiag("setup:pos-load");

   wifi.begin();

   heapDiag("setup:pos-wifi-begin");

   // A pending /ota runs here: after wifi.begin() but before telegram.begin() and
   // telemetry.begin(), so no other TLS client exists yet and the heap is at its cleanest.
   // On failure the normal boot simply continues from telegram.begin() below.
   runPendingOtaIfRequested();

   telegram.begin(persisted.tgUpdateOffset);

   heapDiag("setup:pos-telegram-begin");

   notifier.begin(&telegram, &stateStore, &persisted);
   telemetry.begin();
   ota.begin();

   heapDiag("setup:pos-clients-begin");

   reconcileBoot();

   heapDiag("setup:pos-reconcile");

   lastSample = millis();
   lastAlive = millis();
   lastSavedHead = persisted.tbLogHead;

   printState("Estado inicial:", persisted.power);
   Serial.printf("Pendentes TB: %lu\n", static_cast<unsigned long>(tbQueue.count() - persisted.tbLogHead));

   heapDiag("setup:fim");
}

void loop() {
   const uint32_t now = millis();
   const uint32_t loopStart = now;

   wifi.update(now);

   // Diagnostic-only: log the Wi-Fi connect instant once (rising edge).
   if (!wifiConnectedLogged && WiFi.status() == WL_CONNECTED) {
      wifiConnectedLogged = true;
      heapDiag("evento:wifi-conectado");
   }

   const uint32_t afterWifi = millis();

   if (timeSource.update()) {
      onTimeSynced();
   }
   const uint32_t afterNtp = millis();

   if ((now - lastSample) >= INPUT_SAMPLE_MS) {
      lastSample = now;
      inputs.update(now);

      // A transition is committed only when ALL four channels finished debouncing. Committing on
      // the first channel that confirms (the previous behaviour) mixed the new value of that
      // channel with the still-old values of the others, producing snapshots that never existed
      // - e.g. "grid available + fed by the grid + fed by the offgrid" during a transfer. The
      // edge is not lost: changed() is sticky until this point.
      if (inputs.changed() && inputs.settled()) {
         const PowerState current = inputs.state();

         if (!samePowerState(current, persisted.power)) {
            handleTransition(current, timeSource.now()); // also queues the visual snapshot
            bootNoticeAbandoned = false;                 // a new transition is a fresh chance for the Telegram API
            printState("Transicao:", current);
         }

         inputs.clearChanged();
      }
   }

   // Confirms a rede failure whose grace window has elapsed. Pure state + millis()
   // logic: the rest of the firmware keeps running during the window.
   serviceRedeFailGrace();

   const uint32_t afterSample = millis();

   telemetry.update(now);

   const uint32_t aliveEpoch = timeSource.synced() ? timeSource.now().value : 0;
   telemetry.publishHeartbeat(now, aliveEpoch);
   publishPendingTb();
   const uint32_t afterTb = millis();

   notifier.update();
   handleTelegramCommands(now);

   // Boot notice: exactly one per boot, sent as soon as Telegram is reachable.
   serviceBootNotice();

   // Visual state line: after the boot text and after any accepted change of the signals.
   serviceVisualNotice();

   // Sticker maintenance: delete the FALTA/RETORNO stickers at 36 h and simplify the associated
   // texts. Independent of the visual line, which is never deleted.
   serviceStickerMaint();

   const uint32_t afterTg = millis();

   updateAlive(now);
   printDiagnostics(now);
   const uint32_t afterMisc = millis();

   yield();
   const uint32_t loopEnd = millis();

   // Diagnostic-only: report abnormal cycles (and per-stage split) without
   // changing the ordering or the logic of the loop.
   const uint32_t loopDur = static_cast<uint32_t>(loopEnd - loopStart);

   if (loopDur >= 500) {
      Serial.printf("[%lu] LOOP: ciclo longo=%lums wifi=%lu ntp=%lu sample=%lu tb=%lu tg=%lu misc=%lu\n", static_cast<unsigned long>(loopEnd),
                    static_cast<unsigned long>(loopDur), static_cast<unsigned long>(afterWifi - loopStart), static_cast<unsigned long>(afterNtp - afterWifi),
                    static_cast<unsigned long>(afterSample - afterNtp), static_cast<unsigned long>(afterTb - afterSample),
                    static_cast<unsigned long>(afterTg - afterTb), static_cast<unsigned long>(afterMisc - afterTg));
   }
}