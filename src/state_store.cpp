#include "state_store.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "config.h"

namespace {
bool writeReplacing(const char *tmpPath, const char *finalPath, const void *data, size_t length) {
   File file = LittleFS.open(tmpPath, "w");

   if (!file) {
      return false;
   }

   const size_t written = file.write(reinterpret_cast<const uint8_t *>(data), length);
   file.flush();
   file.close();

   if (written != length) {
      LittleFS.remove(tmpPath);
      return false;
   }

   // LittleFS rename atomically replaces an existing destination.
   if (LittleFS.rename(tmpPath, finalPath)) {
      return true;
   }

   // Defensive fallback only if overwriting is refused.
   LittleFS.remove(finalPath);
   return LittleFS.rename(tmpPath, finalPath);
}
} // namespace

bool StateStore::begin() {
   if (!LittleFS.begin()) {
      // A mount failure is NEVER permission to format: formatting would destroy
      // state.bin and pending.bin (the persisted state and history).
      Serial.println("ERRO: falha ao montar LittleFS (sem formatacao automatica).");
      return false;
   }

   // An orphan temp file must never prevent normal operation.
   if (LittleFS.exists(STATE_TMP)) {
      LittleFS.remove(STATE_TMP);
   }

   return true;
}

bool StateStore::load(PersistedState &state) {
   File file = LittleFS.open(STATE_FILE, "r");

   if (!file) {
      return false;
   }

   const size_t fileSize = file.size();

   // Unknown size: never interpret the image partially (a truncated/corrupt state.bin would load
   // garbage). The caller keeps its defaults and this is reported explicitly.
   if (!StateMigration::isKnownSize(fileSize)) {
      file.close();
      Serial.printf("ERRO: state.bin com tamanho desconhecido (%u bytes); ignorado (defaults).\n", static_cast<unsigned>(fileSize));
      return false;
   }

   uint8_t buffer[sizeof(PersistedState)];
   const size_t readBytes = file.read(buffer, fileSize);
   file.close();

   if (readBytes != fileSize) {
      Serial.println("ERRO: leitura incompleta de state.bin; ignorado (defaults).");
      return false;
   }

   if (!StateMigration::adopt(buffer, fileSize, state)) {
      return false;
   }

   // A legacy image was adopted: migrate it in place, atomically (temp file + rename), so the new
   // fields are persisted and the next boot reads the current size. The original file is preserved
   // until the rename confirms the write.
   if (fileSize != sizeof(PersistedState)) {
      if (writeReplacing(STATE_TMP, STATE_FILE, &state, sizeof(PersistedState))) {
         Serial.printf("STATE: migrado de %u para %u bytes (estado anterior preservado).\n", static_cast<unsigned>(fileSize),
                       static_cast<unsigned>(sizeof(PersistedState)));
      } else {
         Serial.println("AVISO: falha ao gravar a migracao de state.bin; original preservado (tentado no proximo boot).");
      }
   }

   return true;
}

bool StateStore::save(const PersistedState &state) {
   return writeReplacing(STATE_TMP, STATE_FILE, &state, sizeof(PersistedState));
}