#include "telegram_notifier.h"

#include <ESP8266WiFi.h>

#include "secrets.h"
#include "state_store.h"
#include "telegram.h"
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

void TelegramNotifier::stepDelOld() {
   // Only clear an id once Telegram confirmed the deletion; otherwise keep the
   // id, stay in DEL_OLD and retry later.
   if (state_->lastFallStickerId > 0) {
      const int32_t id = state_->lastFallStickerId;
      const uint32_t tDel = millis();

      Serial.printf("[%lu] TG: DEL_FALL inicio id=%ld\n", static_cast<unsigned long>(tDel), static_cast<long>(id));

      const bool ok = telegram_->deleteMessage(id);

      Serial.printf("[%lu] TG: DEL_FALL resultado=%s id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA", static_cast<long>(id),
                    static_cast<unsigned long>(millis() - tDel));

      if (!ok) {
         return;
      }

      state_->lastFallStickerId = 0;
      save();
   } else {
      Serial.printf("[%lu] TG: DEL_FALL skip id=0\n", static_cast<unsigned long>(millis()));
   }

   if (state_->lastReturnStickerId > 0) {
      const int32_t id = state_->lastReturnStickerId;
      const uint32_t tDel = millis();

      Serial.printf("[%lu] TG: DEL_RET inicio id=%ld\n", static_cast<unsigned long>(tDel), static_cast<long>(id));

      const bool ok = telegram_->deleteMessage(id);

      Serial.printf("[%lu] TG: DEL_RET resultado=%s id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA", static_cast<long>(id),
                    static_cast<unsigned long>(millis() - tDel));

      if (!ok) {
         return;
      }

      state_->lastReturnStickerId = 0;
      save();
   } else {
      Serial.printf("[%lu] TG: DEL_RET skip id=0\n", static_cast<unsigned long>(millis()));
   }

   state_->tgPhase = TG_PHASE_SEND_FALL;
   save();
}

void TelegramNotifier::stepSendFall() {
   int32_t messageId = 0;

   const uint32_t tSend = millis();

   Serial.printf("[%lu] TG: SEND_FALL inicio\n", static_cast<unsigned long>(tSend));

   const bool ok = telegram_->sendSticker(TELEGRAM_OLI_STICKER, messageId);

   Serial.printf("[%lu] TG: SEND_FALL resultado=%s message_id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<long>(messageId), static_cast<unsigned long>(millis() - tSend));

   if (!ok) {
      return; // retry later
   }

   state_->lastFallStickerId = messageId;
   state_->lastReturnStickerId = 0;
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

   const uint32_t tSend = millis();

   Serial.printf("[%lu] TG: SEND_RET inicio\n", static_cast<unsigned long>(tSend));

   const bool ok = telegram_->sendSticker(TELEGRAM_HUNO_STICKER, messageId);

   Serial.printf("[%lu] TG: SEND_RET resultado=%s message_id=%ld duracao=%lums\n", static_cast<unsigned long>(millis()), ok ? "OK" : "FALHA",
                 static_cast<long>(messageId), static_cast<unsigned long>(millis() - tSend));

   if (!ok) {
      return;
   }

   state_->lastReturnStickerId = messageId;
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