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
}

void TelegramNotifier::update() {
   if (state_ == nullptr || telegram_ == nullptr || !state_->tgPending) {
      return;
   }

   if (WiFi.status() != WL_CONNECTED) {
      return;
   }

   // Plain coalescing. A forced reboot reconstruction is never suppressed here.
   if (!state_->tgForceNotify && state_->tgNotifiedRede == state_->power.redeDisponivel) {
      state_->tgPending = false;
      save();
      return;
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
   state_->tgNotifiedRede = deliveredValue;
   state_->tgPhase = TG_PHASE_NONE;
   state_->tgForceNotify = false;

   // A newer transition may still be waiting to be delivered.
   state_->tgPending = (state_->tgNotifiedRede != state_->power.redeDisponivel);

   save();
}

void TelegramNotifier::stepDelOld() {
   if (state_->lastFallStickerId > 0) {
      telegram_->deleteMessage(state_->lastFallStickerId);
   }

   if (state_->lastReturnStickerId > 0) {
      telegram_->deleteMessage(state_->lastReturnStickerId);
   }

   state_->lastFallStickerId = 0;
   state_->lastReturnStickerId = 0;
   state_->tgPhase = TG_PHASE_SEND_FALL;
   save();
}

void TelegramNotifier::stepSendFall() {
   int32_t messageId = 0;

   if (!telegram_->sendSticker(TELEGRAM_OLI_STICKER, messageId)) {
      return; // retry later
   }

   state_->lastFallStickerId = messageId;
   state_->lastReturnStickerId = 0;
   state_->tgPhase = TG_PHASE_TEXT_FALL;
   save();
}

void TelegramNotifier::stepTextFall() {
   if (!telegram_->sendText(buildMessage(false))) {
      return;
   }

   finishSequence(false);
}

void TelegramNotifier::stepSendRet() {
   int32_t messageId = 0;

   if (!telegram_->sendSticker(TELEGRAM_HUNO_STICKER, messageId)) {
      return;
   }

   state_->lastReturnStickerId = messageId;
   state_->tgPhase = TG_PHASE_TEXT_RET;
   save();
}

void TelegramNotifier::stepTextRet() {
   if (!telegram_->sendText(buildMessage(true))) {
      return;
   }

   finishSequence(true);
}

String TelegramNotifier::buildMessage(bool returnOfEnergy) const {
   const char *adjective = ADJECTIVES[state_->tgAdjectiveIndex % kAdjectiveCount];
   const char *person = returnOfEnergy ? "Huno" : "Oliver";
   const char *verb = returnOfEnergy ? RETURN_VERBS[state_->tgVerbIndex % kReturnVerbCount] : FAILURE_VERBS[state_->tgVerbIndex % kFailureVerbCount];
   const char *text = returnOfEnergy ? "A LUZ VOLTOU!" : "FALTOU LUZ!";
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