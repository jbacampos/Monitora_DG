#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "config.h"
#include "heap_diag.h"
#include "input_monitor.h"
#include "ota.h"
#include "secrets.h"
#include "state_store.h"
#include "tb_queue.h"
#include "telegram.h"
#include "telegram_notifier.h"
#include "telemetry.h"
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
   // The flags below are RAM only (never persisted), but the id of the current visual
   // message IS persisted (PersistedState::lastVisualMessageId / visualDeletePendingId) so a
   // new boot line can replace the previous one. visualPending is armed by a change of any of
   // the four signals and by the boot notice, and cleared only after the line was delivered
   // (or when the settled state is already the last one reported). Change driven lines never
   // represent a provisional redeDisponivel fall and only go out after the state has been
   // stable for VISUAL_NOTICE_GRACE_MS: see serviceVisualNotice().
   // ---------------------------------------------------------------------------

   // Stability window for the visual line. Every change that arms visualPending restarts
   // it, so a fast sequence (offgrid ON -> both OFF -> gerador ON) coalesces into a single
   // line carrying the final state: no intermediate situation is ever sent. The boot line
   // is exempt and goes out immediately after the boot text.
   constexpr uint32_t VISUAL_NOTICE_GRACE_MS = 3000UL;

   bool visualPending = false;
   bool visualBootPending = false; // boot line: immediate, ignores the stability window
   uint32_t visualArmedMillis = 0;
   bool bootTextSent = false;
   bool visualStateKnown = false;
   PowerState lastVisualState{};

   constexpr char LED_REDE_ON[] = "\xF0\x9F\x9F\xA2";      // 🟢 rede disponivel
   constexpr char LED_ALIM_REDE_ON[] = "\xF0\x9F\x94\xB5"; // 🔵 alimentacao pela rede
   constexpr char LED_OFFGRID_ON[] = "\xF0\x9F\x9F\xA1";   // 🟡 alimentacao pelo offgrid
   constexpr char LED_GERADOR_ON[] = "\xF0\x9F\x94\xB4";   // 🔴 alimentacao pelo gerador
   constexpr char LED_OFF[] = "\xE2\x9A\xAA";              // ⚪ estado OFF/inativo

   // Monotonic start of the redeDisponivel failure grace window for THIS session.
   // Re-armed after a reboot because millis() does not survive a reset.
   uint32_t redeFailGraceMillis = 0;

   void saveState() {
      stateStore.save(persisted);
   }

   void enqueue(const PowerState &power, const Timestamp &ts) {
      PendingRecord record;
      record.timestamp = ts;
      record.power = power;

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
         const uint32_t start = persisted.redeFailStarted.value;
         const uint32_t nowEpoch = timeSource.now().value;

         if (nowEpoch >= start) {
            return static_cast<uint64_t>(nowEpoch - start) * 1000ULL >= REDE_FAIL_GRACE_MS;
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

      enqueue(persisted.power, persisted.redeFailStarted);
      notifier.onRedeTransition(persisted.redeFailStarted);
      saveState();
   }

   void handleTransition(const PowerState &current, const Timestamp &ts) {
      const bool redeChanged = current.redeDisponivel != persisted.power.redeDisponivel;
      const bool previousRede = persisted.power.redeDisponivel;

      // A redeDisponivel 1 -> 0 is not a FALTA yet: it opens a grace window. Until
      // it elapses there is no ThingsBoard event and no Telegram notification.
      if (redeChanged && !current.redeDisponivel) {
         if (!persisted.redeFailGraceActive) {
            persisted.redeFailGraceActive = true;
            persisted.redeFailStarted = ts;
            redeFailGraceMillis = millis();

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

         Serial.printf("[%lu] REDE: queda transitoria descartada (graca cancelada)\n", static_cast<unsigned long>(millis()));

         persisted.power = current;
         saveState();
         return;
      }

      enqueue(current, ts);
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

   void reconcileBoot() {
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

         enqueue(actual, timeSource.now());
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
            persisted.alive.timestamp = timeSource.now();
            persisted.alive.redeDisponivel = true;

            Serial.printf("[%lu] REDE: graca descartada apos reboot (rede voltou)\n", static_cast<unsigned long>(millis()));
         } else {
            // Still down: keep waiting for the remainder of the window.
            redeFailGraceMillis = millis();

            Serial.printf("[%lu] REDE: graca retomada apos reboot\n", static_cast<unsigned long>(millis()));
         }

         // Boot snapshot keeps its usual semantics; no FALTA is recorded here.
         enqueue(actual, timeSource.now());
         saveState();
         return;
      }

      // A fall that is only observed after the reboot must pass the grace window
      // too, so no FALTA is confirmed here.
      if (before && !after) {
         persisted.redeFailGraceActive = true;
         persisted.redeFailStarted = persisted.alive.timestamp; // best estimate of the fall instant
         redeFailGraceMillis = millis();
         persisted.power = actual;

         Serial.printf("[%lu] REDE: queda detectada no boot (graca %lums)\n", static_cast<unsigned long>(millis()),
                       static_cast<unsigned long>(REDE_FAIL_GRACE_MS));

         // Boot snapshot keeps its usual semantics; the FALTA stays deferred until
         // the grace window elapses.
         enqueue(actual, timeSource.now());
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

      enqueue(actual, timeSource.now()); // mandatory boot snapshot
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

   void handleTelegramCommands(uint32_t now) {
      // While a notification sequence is in flight (tgPending with an active
      // phase), its phases must run back to back: polling getUpdates() here would
      // insert a full HTTPS round-trip between every step. Commands are still
      // processed normally as soon as the sequence has finished (phase == NONE).
      const bool notifying = persisted.tgPending && persisted.tgPhase != TG_PHASE_NONE;

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

   // Sends the visual line once per boot (right after the boot text) and whenever a change
   // of the four signals has been settled. A provisional redeDisponivel fall - one that is
   // still inside the grace window - is never represented by a change driven line: it waits
   // until the window is resolved (FALTA confirmed or fall discarded). A discarded fall
   // leaves the state equal to the last one reported, which suppresses the message entirely.
   // While a FALTA/RETORNO sequence is in flight (tgPhase != NONE) the line waits, so the
   // existing coalescing is respected and nothing interleaves with the stickers/texts.
   //
   // Exactly one visual message is kept: the previous one is deleted only AFTER the new
   // one was confirmed (its id is persisted first). If that deletion fails, its id stays
   // persisted (visualDeletePendingId) and no new line is sent until it succeeds, so
   // orphan bubbles never accumulate. Changes arriving meanwhile keep arming visualPending
   // and are coalesced into the current state.
   void serviceVisualNotice() {
      if (!bootTextSent || WiFi.status() != WL_CONNECTED) {
         return;
      }

      if (persisted.tgPhase != TG_PHASE_NONE) {
         return;
      }

      // A previous visual message awaiting its deletion blocks any new visual message:
      // the pending deletion is retried first. The current state stays pending
      // (visualPending) and coalesced, and is sent once the deletion is resolved.
      if (persisted.visualDeletePendingId != 0) {
         if (telegram.deleteMessage(persisted.visualDeletePendingId)) {
            persisted.visualDeletePendingId = 0;
            saveState();

            Serial.printf("[%lu] TG: LEDs antigos apagados (pendente)\n", static_cast<unsigned long>(millis()));
         } else {
            return; // keep the id persisted and retry before sending a new line
         }
      }

      if (!visualPending) {
         return;
      }

      // The boot line mirrors the GPIOs even while a grace window is pending; a change
      // driven line never does.
      if (persisted.redeFailGraceActive && visualStateKnown) {
         return;
      }

      // Stability window. The boot line is exempt, so it follows the boot text right away.
      if (!visualBootPending && static_cast<uint32_t>(millis() - visualArmedMillis) < VISUAL_NOTICE_GRACE_MS) {
         return;
      }

      const PowerState current = inputs.state();

      if (visualStateKnown && samePowerState(current, lastVisualState)) {
         visualPending = false; // the fall was transient: same state as the last one reported
         return;
      }

      // Consumed before the send: a change during the send re-arms the flag.
      visualPending = false;

      int32_t messageId = 0;

      if (telegram.sendText(visualLine(current), messageId) && messageId > 0) {
         const int32_t previousId = persisted.lastVisualMessageId;

         // Persist the new id and the previous one BEFORE deleting: a failure between the
         // two steps can never lose the current visual state.
         persisted.lastVisualMessageId = messageId;
         persisted.visualDeletePendingId = (previousId > 0 && previousId != messageId) ? previousId : 0;
         saveState();

         lastVisualState = current;
         visualStateKnown = true;
         visualBootPending = false;

         Serial.printf("[%lu] TG: LEDs enviados rede=%d alim_rede=%d offgrid=%d gerador=%d id=%ld\n", static_cast<unsigned long>(millis()),
                       current.redeDisponivel ? 1 : 0, current.alimentacaoRede ? 1 : 0, current.alimentacaoOffgrid ? 1 : 0,
                       current.alimentacaoGerador ? 1 : 0, static_cast<long>(messageId));

         // Replace the previous visual message. If the deletion fails, the id stays
         // persisted and the next pass retries it (blocking any new line until then).
         if (persisted.visualDeletePendingId != 0) {
            if (telegram.deleteMessage(persisted.visualDeletePendingId)) {
               persisted.visualDeletePendingId = 0;
               saveState();

               Serial.printf("[%lu] TG: LEDs antigos apagados\n", static_cast<unsigned long>(millis()));
            } else {
               Serial.printf("[%lu] TG: LEDs antigos: apagar falhou id=%ld\n", static_cast<unsigned long>(millis()),
                             static_cast<long>(persisted.visualDeletePendingId));
            }
         }
      } else {
         visualPending = true; // retry with the state detected then
      }
   }

   // One Telegram message per boot, using the normal sendMessage() infrastructure.
   // The bootTextSent flag lives in RAM only, so every reset re-arms it; the loop is the
   // retry mechanism and nothing here blocks the boot (setup() never waits for Telegram).
   // An OTA reboot is reported by the ESP8266 as a generic software restart, so it is
   // identified by the marker persisted before the update started (pendingBootReason).
   void serviceBootNotice() {
      if (bootTextSent || WiFi.status() != WL_CONNECTED) {
         return;
      }

      const bool fromUpdate = (persisted.pendingBootReason == BOOT_REASON_OTA);
      const String reason = fromUpdate ? String("Update/OTA") : ESP.getResetReason();
      const String message = String("Monitora_DG reiniciado.\nVersão: ") + FIRMWARE_VERSION + "\nMotivo: " + reason;

      if (telegram.sendText(message)) {
         bootTextSent = true;
         // The visual line follows the boot text (serviceVisualNotice(), same loop pass) and
         // is exempt from the stability window.
         visualPending = true;
         visualBootPending = true;

         // Consume the marker only after the message was actually delivered, then persist
         // the cleared state so the next boot reports the real reset reason again.
         if (fromUpdate) {
            persisted.pendingBootReason = BOOT_REASON_NONE;
            saveState();
         }

         Serial.printf("[%lu] TG: notificacao de boot enviada (motivo=%s)\n", static_cast<unsigned long>(millis()),
                       fromUpdate ? "Update/OTA" : "reset");
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

      Serial.printf("DIAG up=%lus wifi=%d rssi=%d ntp=%d rede=%d pend=%lu head=%lu gap=%u tgPend=%d tgNotif=%d tgForce=%d tgPhase=%s fallId=%ld retId=%ld visId=%ld visDelId=%ld\n",
                    static_cast<unsigned long>(now / 1000UL), WiFi.status() == WL_CONNECTED ? 1 : 0, WiFi.RSSI(), timeSource.synced() ? 1 : 0,
                    persisted.power.redeDisponivel ? 1 : 0, static_cast<unsigned long>(count - persisted.tbLogHead),
                    static_cast<unsigned long>(persisted.tbLogHead), persisted.tbGapCount, persisted.tgPending ? 1 : 0, persisted.tgNotifiedRede ? 1 : 0,
                    persisted.tgForceNotify ? 1 : 0, phaseName(persisted.tgPhase), static_cast<long>(persisted.lastFallStickerId),
                    static_cast<long>(persisted.lastReturnStickerId), static_cast<long>(persisted.lastVisualMessageId),
                    static_cast<long>(persisted.visualDeletePendingId));
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

      if (inputs.changed()) {
         const PowerState current = inputs.state();

         if (!samePowerState(current, persisted.power)) {
            handleTransition(current, timeSource.now());
            visualArmedMillis = millis(); // every change restarts the stability window
            visualPending = true;         // the settled state is reported by serviceVisualNotice()
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