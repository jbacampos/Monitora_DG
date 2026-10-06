#pragma once

#include <Arduino.h>

class Ota {
public:
   // Result of comparing FIRMWARE_VERSION (config.h) with the latest release tag on GitHub.
   enum class VersionStatus : uint8_t {
      UpToDate,        // the remote version is EQUAL TO or LOWER than the installed one
      UpdateAvailable, // the remote version is strictly greater
      Unknown,         // the remote version could not be obtained or compared
   };

   // Asks OTA_VERSION_URL for the latest release and compares its tag_name with
   // FIRMWARE_VERSION. On UpdateAvailable, remoteVersion receives the NORMALISED version
   // (digits and dots only). Every failure (network, HTTP, JSON, missing tag_name, invalid
   // version) returns Unknown: the caller must then leave the OTA request untouched, so a
   // temporary network problem can never trigger an update.
   VersionStatus checkVersion(String &remoteVersion);

   bool begin();
   bool run();
};
