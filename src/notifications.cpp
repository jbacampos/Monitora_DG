#include "notifications.h"

#include <Arduino.h>

#include "telegram.h"
#include "secrets.h"

const char *Notifications::adjectives_[] = {
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

const char *Notifications::failureVerbs_[] = {
   "adverte", "apercebeu-se", "aponta", "assinala", "atesta", "cientifica",
   "consigna", "constatou", "identificou", "lamenta", "observa", "previne",
   "registra", "relata", "ressalta", "salienta", "verificou"
};

const char *Notifications::returnVerbs_[] = {
   "anuncia", "assegura", "avisa", "comemora", "comunica", "confirma",
   "declara", "divulga", "faz saber", "garante", "informa", "notifica",
   "participa", "proclama", "reporta", "torna público", "tranquiliza"
};

void Notifications::begin(
   Telegram *telegram,
   PersistedState *persisted) {

   telegram_ = telegram;
   persisted_ = persisted;

   for (size_t i = 0; i < adjectiveCount_; ++i) {
      adjectiveOrder_[i] = static_cast<int>(i);
   }

   for (size_t i = 0; i < failureVerbCount_; ++i) {
      failureOrder_[i] = static_cast<int>(i);
   }

   for (size_t i = 0; i < returnVerbCount_; ++i) {
      returnOrder_[i] = static_cast<int>(i);
   }

   randomSeed(ESP.getCycleCount() ^ micros());
}

void Notifications::shuffle(int *order, size_t count) {
   for (size_t i = count - 1; i > 0; --i) {
      const size_t j = static_cast<size_t>(random(static_cast<long>(i + 1)));
      const int temp = order[i];
      order[i] = order[j];
      order[j] = temp;
   }
}

int Notifications::nextIndex(
   int *order,
   size_t count,
   size_t &position) {

   if (position >= count) {
      shuffle(order, count);
      position = 0;
   }

   return order[position++];
}

String Notifications::makeMessage(bool returnOfEnergy) {
   const int adjectiveIndex =
      nextIndex(adjectiveOrder_, adjectiveCount_, adjectivePosition_);

   const char *adjective = adjectives_[adjectiveIndex];

   const char *person = returnOfEnergy ? "Huno" : "Oliver";

   const int verbIndex = returnOfEnergy
      ? nextIndex(returnOrder_, returnVerbCount_, returnPosition_)
      : nextIndex(failureOrder_, failureVerbCount_, failurePosition_);

   const char *verb = returnOfEnergy
      ? returnVerbs_[verbIndex]
      : failureVerbs_[verbIndex];

   const char *text = returnOfEnergy
      ? "A LUZ VOLTOU!"
      : "FALTOU LUZ!";

   const char *emoji = returnOfEnergy ? "💡" : "🕯️";

   char buffer[256];

   snprintf(
      buffer,
      sizeof(buffer),
      "O %s Sr. %s %s:\n%s %s %s",
      adjective,
      person,
      verb,
      emoji,
      text,
      emoji
   );

   return String(buffer);
}

void Notifications::onPowerStateChanged(
   const PowerState &previous,
   const PowerState &current) {

   if (!telegram_ || !persisted_) {
      return;
   }

   if (previous.redeDisponivel == current.redeDisponivel) {
      return;
   }

   if (!current.redeDisponivel) {
      // Policy:
      // - a power-failure notification deletes the previous failure sticker
      // - the new failure sticker becomes the stored "last failure sticker"
      if (persisted_->lastFallStickerId > 0) {
         telegram_->deleteMessage(persisted_->lastFallStickerId);
      }

      int32_t newStickerId = 0;

      if (telegram_->sendSticker(
             TELEGRAM_OLI_STICKER,
             newStickerId)) {

         persisted_->lastFallStickerId = newStickerId;
      }

      telegram_->sendText(makeMessage(false));
      return;
   }

   // Policy:
   // - return notification does NOT delete the previous failure sticker
   // - it also does NOT replace the stored failure sticker ID
   int32_t ignoredStickerId = 0;
   telegram_->sendSticker(
      TELEGRAM_HUNO_STICKER,
      ignoredStickerId
   );

   telegram_->sendText(makeMessage(true));
}
