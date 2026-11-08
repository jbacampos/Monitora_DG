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

   // Asks OTA_VERSION_URL for the latest release, compares its tag_name with FIRMWARE_VERSION, and
   // (when the remote version is newer) resolves the STABLE download endpoint of the release asset
   // named OTA_ASSET_NAME into `assetUrl` (the asset API URL, never the temporary signed CDN URL).
   // On UpdateAvailable, remoteVersion receives the NORMALISED version (digits and dots only). Every
   // failure (network, HTTP, JSON, missing tag_name, invalid version) returns Unknown and leaves the
   // OTA request untouched, so a temporary network problem can never trigger an update. When the
   // version is newer but the asset is absent or ambiguous, assetUrl stays EMPTY: the caller must
   // abort the update explicitly (it never falls back to the previous download URL).
   VersionStatus checkVersion(String &remoteVersion, String &assetUrl);

   bool begin();

   // Downloads through the STABLE asset API endpoint resolved by checkVersion() and stored in the
   // persisted state, then flashes it with ESPhttpUpdate.update(HTTPClient&). Uses the asset endpoint
   // plus "Accept: application/octet-stream" (never the github.com redirect chain) and NO
   // Authorization (the PAT must not reach github.com or the CDN). Returns false, without rebooting,
   // on any failure; an empty/absent assetUrl aborts.
   bool run(const char *assetUrl);
};
