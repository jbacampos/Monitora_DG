#include "telegram_notifier.h"

#include <ESP8266WiFi.h>

#include "config.h"
#include "secrets.h"
#include "state_store.h"
#include "telegram.h"
#include "tg_maint.h"
#include "time_source.h"

namespace {
// Preserved verbatim from the existing draft. Do not edit the contents.
const char *const ADJECTIVES[] = {
   "abnegado", "adamantino", "afável", "aguerrido", "altruísta", "aquilatado", "arguto", "astuto",
   "atilado", "austero", "benevolente", "benemérito", "brioso", "carismático", "cavalheiresco",
   "circunspecto", "conciliador", "consciencioso", "conspícuo", "competente", "cordato", "culto",
   "dedicado", "denodado", "desassombrado", "destemido", "determinado", "devotado", "digno", "diligente",
   "distinto", "douto", "egrégio", "eminente", "escorreito", "equilibrado", "erudito", "escrupuloso",
   "esforçado", "estoico", "exemplar", "exímio", "fidalgo", "galhardo", "generoso",
   "honrado", "idôneo", "ilustrado", "ilustre", "impávido", "impoluto", "incansável", "incorruptível",
   "indômito", "inabalável", "inatacável", "inquebrantável", "insigne", "intemerato",
   "intrépido", "íntegro", "judicioso", "laborioso", "laudável", "lhano", "literato", "luminar",
   "magnânimo", "meritório", "meticuloso", "notável", "operoso", "percuciente", "perseverante",
   "perspicaz", "ponderado", "preclaro", "prestigioso", "prestimoso", "probo", "proficiente",
   "prudente", "resoluto", "respeitável", "sábio", "sagaz", "sensato", "sereno", "tenaz", "valoroso",
   "venerando", "versado", "virtuoso", "zeloso"
};

const char *const FAILURE_VERBS[] = {
   "adverte", "apercebeu-se", "aponta", "assinala", "atesta", "cientifica",
   "consigna", "constatou", "identificou", "lamenta", "observa", "previne",
   "registra", "relata", "ressalta", "salienta", "verificou"
};

const char *const RETURN_VERBS[] = {
   "anuncia", "assegura", "avisa", "comemora", "comunica", "confirma",
   "declara", "divulga", "faz saber", "garante", "informa", "notifica",
   "participa", "proclama", "reporta", "torna público", "tranquiliza"
};

static_assert(sizeof(ADJECTIVES) / sizeof(ADJECTIVES[0]) == TelegramNotifier::kAdjectiveCount, "adjective list size mismatch");
static_assert(sizeof(FAILURE_VERBS) / sizeof(FAILURE_VERBS[0]) == TelegramNotifier::kFailureVerbCount, "failure verb list size mismatch");
static_assert(sizeof(RETURN_VERBS) / sizeof(RETURN_VERBS[0]) == TelegramNotifier::kReturnVerbCount, "return verb list size mismatch");

constexpr char EMOJI_FALL[] = "\xF0\x9F\x95\xAF\xEF\xB8\x8F"; // candle
constexpr char EMOJI_RETURN[] = "\xF0\x9F\x92\xA1";           // bulb
constexpr char EMOJI_CLOCK[] = "\xF0\x9F\x95\x92";            // clock

// Diagnostic-only mirror of the persisted phase names (see main.cpp phaseName()).
// Does not change any behaviour; used purely for Serial instrumentation.
const char *tgPhaseName(uint8_t phase) {
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
} // namespace

void TelegramNotifier::begin(Telegram *telegram, StateStore *store, PersistedState *state) {
   telegram_ = telegram;
   store_ = store;
   state_ = state;

   for (size_t i = 0; i < kAdjectiveCount; ++i) {
      adjectiveOrder_[i] = static_cast<int>(i);
   }

   for (size_t i = 0; i < kFailureVerbCount; ++i) {
      failureOrder_[i] = static_cast<int>(i);
   }

   for (size_t i = 0; i < kReturnVerbCount; ++i) {
      returnOrder_[i] = static_cast<int>(i);
   }

   randomSeed(ESP.getCycleCount() ^ micros());
}

void TelegramNotifier::save() {
   if (store_ != nullptr && state_ != nullptr) {
      store_->save(*state_);
   }
}

void TelegramNotifier::onRedeTransition(const Timestamp &ts) {
   if (state_ == nullptr) {
      return;
   }

   state_->tgPending = true;
   state_->tgTimestamp = ts;
   // Normal transitions keep the plain coalescing semantics.
   state_->tgForceNotify = false;
}

void TelegramNotifier::onBootReturnReconstruction(const Timestamp &ts) {
   if (state_ == nullptr) {
      return;
   }

   state_->tgPending = true;
   state_->tgTimestamp = ts;
   state_->tgForceNotify = true;
   // A mandatory RETORNO must not inherit an incompatible FALTA phase.
   state_->tgPhase = TG_PHASE_NONE;
}

void TelegramNotifier::update() {
   if (state_ == nullptr || telegram_ == nullptr) {
      return;
   }

   // Reconciliation via the STATE is used for the RETURN direction only: Telegram can
   // be left reporting "rede indisponivel" with no event to carry it, because a fall
   // discarded inside the grace window is never reported as a transition (main.cpp
   // handleTransition returns without notifying). That divergence must be acted on,
   // otherwise the missing RETORNO would never be sent.
   //
   // The opposite direction is deliberately NOT derived from the state. A fall is
   // announced only once the grace window confirms it (main.cpp
   // serviceRedeFailGrace -> onRedeTransition): while the grace is still running
   // persisted.power.redeDisponivel is already false, so acting on that divergence
   // would start the FALTA — deleting the previous stickers — for a fall that may
   // still be transient. A FALTA is therefore only ever started by the confirmation
   // event.
   const bool returnPending = (!state_->tgForceNotify && !state_->tgNotifiedRede && state_->power.redeDisponivel);

   if (!state_->tgPending && !returnPending) {
      return;
   }

   if (WiFi.status() != WL_CONNECTED) {
      return;
   }

   // Plain coalescing. A forced reboot reconstruction is never suppressed here.
   //
   // It is only valid when NO sequence is in flight: with an active phase Telegram
   // does not yet reflect tgNotifiedRede (a sequence can be half sent, e.g. the
   // FALTA sticker already posted), so the state must never be considered already
   // delivered — otherwise the sequence would be abandoned mid-way and the phase
   // would stay stuck forever.
   if (state_->tgPhase == TG_PHASE_NONE && !state_->tgForceNotify && state_->tgNotifiedRede == state_->power.redeDisponivel) {
      state_->tgPending = false;
      save();
      return;
   }

   // Diagnostic-only: make sure a resumed sequence (phase restored from Flash)
   // still has a start reference for its total-time report.
   if (seqStartMillis_ == 0) {
      seqStartMillis_ = millis();
   }

   // Retry gate. A step that failed is not repeated on the next loop pass: each attempt is a
   // full TLS round-trip and a group chat accepts only 20 messages per minute, so retrying
   // every pass made a transient error worse (429 flood) instead of fixing it. Any phase change
   // clears the gate, so a sequence that is actually advancing is never delayed.
   const uint8_t phase = state_->tgPhase;

   if (phase != stepPhase_) {
      stepPhase_ = phase;
      stepAttempts_ = 0;
      retryDelayMs_ = 0;
      retryPending_ = false;
   }

   if (retryPending_ && static_cast<int32_t>(millis() - nextAttemptMillis_) < 0) {
      return;
   }

   const uint8_t phaseBefore = state_->tgPhase;

   switch (state_->tgPhase) {
      case TG_PHASE_NONE:
         startSequence();
         break;
      case TG_PHASE_DEL_OLD:
         stepDelOld();
         break;
      case TG_PHASE_SEND_FALL:
         stepSendFall();
         break;
      case TG_PHASE_TEXT_FALL:
         stepTextFall();
         break;
      case TG_PHASE_SEND_RET:
         stepSendRet();
         break;
      case TG_PHASE_TEXT_RET:
         stepTextRet();
         break;
      default:
         state_->tgPhase = TG_PHASE_NONE;
         break;
   }

   // A step that did not move the sequence forward failed. If Telegram refused it with an
   // error that no retry can change (bad token, chat not found, no rights to send, invalid
   // sticker), the sequence is abandoned instead of being retried forever: the persisted
   // phase would never be released and the command poll waits for it. Any other failure is
   // retried after the backoff delay below.
   if (state_->tgPhase == phaseBefore) {
      if (telegram_->lastErrorFatal()) {
         abortSequence("erro fatal da API");
      } else {
         scheduleRetry();
      }
   } else {
      stepPhase_ = state_->tgPhase;
      stepAttempts_ = 0;
      retryDelayMs_ = 0;
      retryPending_ = false;
   }
}

void TelegramNotifier::scheduleRetry() {
   if (stepAttempts_ < 0xFFFF) {
      stepAttempts_++;
   }

   // 3 s, 6 s, 12 s, 24 s, 48 s, 60 s, 60 s, ...
   uint16_t shifts = (stepAttempts_ > 0) ? static_cast<uint16_t>(stepAttempts_ - 1) : 0;

   if (shifts > 6) {
      shifts = 6;
   }

   uint32_t delayMs = TELEGRAM_RETRY_BACKOFF_MIN_MS << shifts;

   if (delayMs > TELEGRAM_RETRY_BACKOFF_MAX_MS) {
      delayMs = TELEGRAM_RETRY_BACKOFF_MAX_MS;
   }

   // Telegram's own retry_after (429 flood limit) wins when it asks for more, bounded by
   // TELEGRAM_RETRY_AFTER_MAX_S.
   uint32_t retryAfterMs = static_cast<uint32_t>(telegram_->lastRetryAfterSeconds()) * 1000UL;

   if (retryAfterMs > (TELEGRAM_RETRY_AFTER_MAX_S * 1000UL)) {
      retryAfterMs = TELEGRAM_RETRY_AFTER_MAX_S * 1000UL;
   }

   if (retryAfterMs > delayMs) {
      delayMs = retryAfterMs;
   }

   retryPending_ = true;
   retryDelayMs_ = delayMs;
   nextAttemptMillis_ = millis() + delayMs;

   Serial.printf("[%lu] TG: retry fase=%s tentativa=%u em=%lums error_code=%d\n", static_cast<unsigned long>(millis()), tgPhaseName(state_->tgPhase),
                 stepAttempts_, static_cast<unsigned long>(retryDelayMs_), telegram_->lastErrorCode());

   // Attempt budget: a step that keeps failing is given up on, so the phase can never park the
   // command poll forever.
   if (stepAttempts_ >= TELEGRAM_MAX_STEP_ATTEMPTS) {
      abortSequence("tentativas esgotadas");
   }
}

void TelegramNotifier::startSequence() {
   const bool retorno = state_->power.redeDisponivel;

   const uint32_t tStart = millis();
   seqStartMillis_ = tStart;

   Serial.printf("[%lu] TG: inicio %s\n", static_cast<unsigned long>(tStart), retorno ? "RETORNO" : "FALTA");
   Serial.printf("[%lu] TG: target rede=%d\n", static_cast<unsigned long>(tStart), retorno ? 1 : 0);
   Serial.printf("[%lu] TG: tgNotifiedRede=%d\n", static_cast<unsigned long>(tStart), state_->tgNotifiedRede ? 1 : 0);
   Serial.printf("[%lu] TG: tgForceNotify=%d\n", static_cast<unsigned long>(tStart), state_->tgForceNotify ? 1 : 0);
   Serial.printf("[%lu] TG: tgPending=%d\n", static_cast<unsigned long>(tStart), state_->tgPending ? 1 : 0);
   Serial.printf("[%lu] TG: tgPhase=%s\n", static_cast<unsigned long>(tStart), tgPhaseName(state_->tgPhase));
   Serial.printf("[%lu] TG: timestamp evento kind=%d value=%lu\n", static_cast<unsigned long>(tStart), static_cast<int>(state_->tgTimestamp.kind),
                 static_cast<unsigned long>(state_->tgTimestamp.value));

   pickIndices(retorno);

   state_->tgPhase = retorno ? TG_PHASE_SEND_RET : TG_PHASE_DEL_OLD;
   save();
}

void TelegramNotifier::pickIndices(bool returnOfEnergy) {
   state_->tgAdjectiveIndex = static_cast<uint8_t>(nextIndex(adjectiveOrder_, kAdjectiveCount, adjectivePosition_));

   state_->tgVerbIndex = returnOfEnergy ? static_cast<uint8_t>(nextIndex(returnOrder_, kReturnVerbCount, returnPosition_))
                                        : static_cast<uint8_t>(nextIndex(failureOrder_, kFailureVerbCount, failurePosition_));
}

void TelegramNotifier::finishSequence(bool deliveredValue) {
   const uint32_t tDone = millis();
   const uint32_t total = static_cast<uint32_t>(tDone - seqStartMillis_);

   state_->tgNotifiedRede = deliveredValue;
   state_->tgPhase = TG_PHASE_NONE;
   state_->tgForceNotify = false;

   // A newer transition may still be waiting to be delivered.
   state_->tgPending = (state_->tgNotifiedRede != state_->power.redeDisponivel);

   save();

   Serial.printf("[%lu] TG: %s total=%lums\n", static_cast<unsigned long>(tDone), deliveredValue ? "RETORNO concluido" : "FALTA concluida",
                 static_cast<unsigned long>(total));
}

void TelegramNotifier::abortSequence(const char *reason) {
   const uint32_t tDone = millis();
   const uint32_t total = static_cast<uint32_t>(tDone - seqStartMillis_);

   Serial.printf("[%lu] TG: sequencia ABORTADA motivo=%s fase=%s error_code=%d fatal=%s total=%lums\n", static_cast<unsigned long>(tDone), reason,
                 tgPhaseName(state_->tgPhase), telegram_->lastErrorCode(), telegram_->lastErrorFatal() ? "sim" : "nao",
                 static_cast<unsigned long>(total));

   // The current direction is marked as handled so the loop does not immediately restart the
   // same doomed sequence (a returnPending or a coalesced pending would re-arm it forever).
   // Nothing is claimed as delivered: Telegram reflects it on the next real transition.
   state_->tgNotifiedRede = state_->power.redeDisponivel;
   state_->tgPhase = TG_PHASE_NONE;
   state_->tgForceNotify = false;
   state_->tgPending = false;

   save();
}

void TelegramNotifier::stepDelOld() {
   // Only clear an id once the deletion was resolved: Telegram confirmed it, the message was
   // already gone, or Telegram refused it permanently (message older than 48 h / bot without
   // can_delete_messages). An outright refusal used to keep the sequence parked in DEL_OLD
   // forever - and with it the getUpdates() poll, so /reboot and /ota went dead while
   // ThingsBoard kept working. A transient failure still keeps the id and the phase.
   if (!resolveStickerDelete(state_->lastFallStickerId, "DEL_FALL")) {
      return;
   }

   if (!resolveStickerDelete(state_->lastReturnStickerId, "DEL_RET")) {
      return;
   }

   // Both stickers of the previous cycle were resolved (deleted, already gone or refused): their
   // preventive-deletion tracking is reset, since they no longer exist. A new cycle starts them
   // again on the next FALTA/RETORNO sends.
   state_->fallStickerSentEpoch = 0;
   state_->fallStickerNextAttempt = 0;
   state_->fallStickerMaint = static_cast<uint8_t>(MaintState::None);
   state_->retStickerSentEpoch = 0;
   state_->retStickerNextAttempt = 0;
   state_->retStickerMaint = static_cast<uint8_t>(MaintState::None);

   state_->tgPhase = TG_PHASE_SEND_FALL;
   save();
}

bool TelegramNotifier::resolveStickerDelete(int32_t &id, const char *label) {
   if (id <= 0) {
      Serial.printf("[%lu] TG: %s skip id=0\n", static_cast<unsigned long>(millis()), label);
      return true;
   }

   const int32_t target = id;
   const uint32_t tDel = millis();

   Serial.printf("[%lu] TG: %s inicio id=%ld\n", static_cast<unsigned long>(millis()), label, static_cast<long>(target));

   const Telegram::DeleteResult result = telegram_->deleteMessage(target);

   const char *verdict = "FALHA";

   switch (result) {
      case Telegram::DeleteResult::Deleted:
         verdict = "OK";
         break;
      case Telegram::DeleteResult::NotFound:
         verdict = "JA_INEXISTENTE";
         break;
      case Telegram::DeleteResult::Impossible:
         verdict = "IMPOSSIVEL";
         break;
      case Telegram::DeleteResult::Failed:
         break;
   }

   Serial.printf("[%lu] TG: %s resultado=%s id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), label, verdict, static_cast<long>(target),
                 static_cast<unsigned long>(millis() - tDel));

   if (result == Telegram::DeleteResult::Failed) {
      return false; // keep the id persisted and stay in the phase; retried later
   }

   if (result == Telegram::DeleteResult::Impossible) {
      // The old sticker stays in the chat (Telegram will never delete it), but the sequence
      // must go on: blocking here also blocked the command poll.
      Serial.printf("[%lu] TG: %s descartado sem apagar (exclusao impossivel) id=%ld\n", static_cast<unsigned long>(millis()), label,
                    static_cast<long>(target));
   }

   id = 0;
   save();
   return true;
}

void TelegramNotifier::stepSendFall() {
   int32_t messageId = 0;
   uint32_t dateEpoch = 0;

   const uint32_t tSend = millis();

   Serial.printf("[%lu] TG: SEND_FALL inicio\n", static_cast<unsigned long>(tSend));

   const bool ok = telegram_->sendSticker(TELEGRAM_OLI_STICKER, messageId, dateEpoch);

   Serial.printf("[%lu] TG: SEND_FALL resultado=%s message_id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<long>(messageId), static_cast<unsigned long>(millis() - tSend));

   if (!ok) {
      return; // retry later
   }

   state_->lastFallStickerId = messageId;
   state_->lastReturnStickerId = 0;

   // A fresh FALTA sticker: start its own preventive-deletion cycle from the API-confirmed send
   // instant. The RETORNO sticker of the previous cycle was already deleted by DEL_OLD.
   state_->fallStickerSentEpoch = dateEpoch;
   state_->fallStickerNextAttempt = 0;
   state_->fallStickerMaint = static_cast<uint8_t>(MaintState::Waiting);

   state_->retStickerSentEpoch = 0;
   state_->retStickerNextAttempt = 0;
   state_->retStickerMaint = static_cast<uint8_t>(MaintState::None);

   state_->tgPhase = TG_PHASE_TEXT_FALL;
   save();
}

void TelegramNotifier::stepTextFall() {
   const uint32_t tText = millis();

   Serial.printf("[%lu] TG: TEXT_FALL inicio\n", static_cast<unsigned long>(tText));

   const bool ok = telegram_->sendText(buildMessage(false));

   Serial.printf("[%lu] TG: TEXT_FALL resultado=%s duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<unsigned long>(millis() - tText));

   if (!ok) {
      return;
   }

   finishSequence(false);
}

void TelegramNotifier::stepSendRet() {
   int32_t messageId = 0;
   uint32_t dateEpoch = 0;

   const uint32_t tSend = millis();

   Serial.printf("[%lu] TG: SEND_RET inicio\n", static_cast<unsigned long>(tSend));

   const bool ok = telegram_->sendSticker(TELEGRAM_HUNO_STICKER, messageId, dateEpoch);

   Serial.printf("[%lu] TG: SEND_RET resultado=%s message_id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<long>(messageId), static_cast<unsigned long>(millis() - tSend));

   if (!ok) {
      return;
   }

   state_->lastReturnStickerId = messageId;

   // A fresh RETORNO sticker: start its own preventive-deletion cycle from the confirmed send
   // instant. The FALTA sticker is preserved (RETORNO never deletes it).
   state_->retStickerSentEpoch = dateEpoch;
   state_->retStickerNextAttempt = 0;
   state_->retStickerMaint = static_cast<uint8_t>(MaintState::Waiting);

   state_->tgPhase = TG_PHASE_TEXT_RET;
   save();
}

void TelegramNotifier::stepTextRet() {
   const uint32_t tText = millis();

   Serial.printf("[%lu] TG: TEXT_RET inicio\n", static_cast<unsigned long>(tText));

   const bool ok = telegram_->sendText(buildMessage(true));

   Serial.printf("[%lu] TG: TEXT_RET resultado=%s duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<unsigned long>(millis() - tText));

   if (!ok) {
      return;
   }

   finishSequence(true);
}

String TelegramNotifier::buildMessage(bool returnOfEnergy) const {
   const char *adjective = ADJECTIVES[state_->tgAdjectiveIndex % kAdjectiveCount];
   const char *person = returnOfEnergy ? "Huno" : "Oliver";
   const char *verb = returnOfEnergy ? RETURN_VERBS[state_->tgVerbIndex % kReturnVerbCount] : FAILURE_VERBS[state_->tgVerbIndex % kFailureVerbCount];
   const char *text = returnOfEnergy ? "<b>A LUZ VOLTOU!</b>" : "<b>FALTOU LUZ!</b>";
   const char *emoji = returnOfEnergy ? EMOJI_RETURN : EMOJI_FALL;

   char when[40];

   if (state_->tgTimestamp.kind == TsKind::Epoch) {
      TimeSource::formatBrasilia(state_->tgTimestamp.value, when, sizeof(when));
   } else {
      snprintf(when, sizeof(when), "horário desconhecido");
   }

   char buffer[320];
   snprintf(buffer, sizeof(buffer), "O %s Sr. %s %s:\n%s %s %s\n%s %s", adjective, person, verb, emoji, text, emoji, EMOJI_CLOCK, when);

   return String(buffer);
}

int TelegramNotifier::nextIndex(int *order, size_t count, size_t &position) {
   if (position >= count) {
      shuffle(order, count);
      position = 0;
   }

   return order[position++];
}

void TelegramNotifier::shuffle(int *order, size_t count) {
   for (size_t i = count - 1; i > 0; --i) {
      const size_t j = static_cast<size_t>(random(static_cast<long>(i + 1)));
      const int temp = order[i];
      order[i] = order[j];
      order[j] = temp;
   }
}