#pragma once

// Pure Telegram command classification (no Arduino, no network, no allocation), covered by the
// host tests in test/host together with the rest of the pure logic.
//
// Telegram::processUpdates() normalises the raw message text first (trim + lowercase), exactly as
// it always did, and then delegates the mapping to classifyTgCommand(). Keeping the mapping here
// lets the host tests cover the recognition of every command without an Arduino/TLS build, and
// keeps the command set in a single place.

#include <stdint.h>
#include <string.h>

enum class TgCommand : uint8_t {
   None = 0, // not a recognised command
   Reboot,   // "/reboot"
   Ota,      // "/ota"
   Status    // "/s"
};

// Maps an ALREADY NORMALISED command text (trimmed and lowercased by the caller) to a command.
// The match is exact: a command carrying arguments ("/s now") is not recognised, which is the
// same rule the previous string comparisons used for "/reboot" and "/ota".
inline TgCommand classifyTgCommand(const char *normalizedText) {
   if (normalizedText == nullptr) {
      return TgCommand::None;
   }

   if (strcmp(normalizedText, "/reboot") == 0) {
      return TgCommand::Reboot;
   }

   if (strcmp(normalizedText, "/ota") == 0) {
      return TgCommand::Ota;
   }

   if (strcmp(normalizedText, "/s") == 0) {
      return TgCommand::Status;
   }

   return TgCommand::None;
}
