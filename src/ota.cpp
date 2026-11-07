#include "ota.h"

#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "secrets.h"
#include "time_source.h"

namespace {
   // ---------------------------------------------------------------------------
   // Version handling.
   //
   // FIRMWARE_VERSION (config.h) is MAJOR.MINOR.PATCH and the release tags use exactly
   // the same format with an optional leading "v"/"V" (the repository publishes both:
   // v1.0.0 and V0.1.0). Nothing else is accepted here: this project has no pre-release
   // or build metadata, so none is interpreted. Absent components default to 0.
   // ---------------------------------------------------------------------------

   struct Version {
      uint16_t parts[3];
   };

   // Accepts MAJOR[.MINOR[.PATCH]] with an optional "v"/"V" prefix. Returns false for an
   // empty input, a non-digit, an empty component, a component longer than five digits, a
   // component above 65535, trailing garbage or a fourth component.
   constexpr bool parseVersion(const char *text, Version &version) {
      version = {{0, 0, 0}};

      if (text == nullptr) {
         return false;
      }

      if (*text == 'v' || *text == 'V') {
         ++text; // GitHub tag prefix
      }

      for (uint8_t index = 0; index < 3; ++index) {
         uint32_t value = 0;
         uint8_t digits = 0;

         while (*text >= '0' && *text <= '9') {
            value = value * 10U + static_cast<uint32_t>(*text - '0');
            ++digits;
            ++text;

            if (digits > 5 || value > 65535U) {
               return false;
            }
         }

         if (digits == 0) {
            return false; // empty component ("", "1..0", "1.")
         }

         version.parts[index] = static_cast<uint16_t>(value);

         if (*text == '\0') {
            return true; // the remaining components stay 0: "1" equals "1.0.0"
         }

         if (*text != '.' || index == 2) {
            return false; // trailing garbage ("1.0.0-rc1") or a fourth component ("1.0.0.1")
         }

         ++text;
      }

      return false;
   }

   // Semantic comparison of two successfully parsed versions: -1, 0 or +1.
   constexpr int compareVersions(const Version &a, const Version &b) {
      for (uint8_t index = 0; index < 3; ++index) {
         if (a.parts[index] != b.parts[index]) {
            return (a.parts[index] < b.parts[index]) ? -1 : 1;
         }
      }

      return 0;
   }

   // True when both strings parse and compareVersions() equals `expected`.
   [[maybe_unused]] constexpr bool versionsCompare(const char *a, const char *b, int expected) {
      Version va = {{0, 0, 0}};
      Version vb = {{0, 0, 0}};

      if (!parseVersion(a, va) || !parseVersion(b, vb)) {
         return false;
      }

      return compareVersions(va, vb) == expected;
   }

   // Build-time proof of the comparison rules: a version regression then fails the build
   // instead of shipping as a wrong update decision.
   static_assert(versionsCompare("1.0.9", "1.0.10", -1), "1.0.9 must be lower than 1.0.10");
   static_assert(versionsCompare("1.1.0", "1.0.99", 1), "1.1.0 must be greater than 1.0.99");
   static_assert(versionsCompare("2.0.0", "1.99.99", 1), "2.0.0 must be greater than 1.99.99");
   static_assert(versionsCompare("1.0.0", "v1.0.0", 0), "1.0.0 must equal v1.0.0");
   static_assert(versionsCompare("1.0.0", "V1.0.0", 0), "the tag prefix may be uppercase (V0.1.0)");
   static_assert(versionsCompare("1", "1.0.0", 0), "absent components default to 0");
   static_assert(versionsCompare("1.0.0", "1.0.0", 0), "equal versions compare equal");
   static_assert(!versionsCompare("v1.0.0-rc1", "1.0.0", 0), "pre-release tags are rejected");
   static_assert(!versionsCompare("abc", "1.0.0", 0), "non-numeric text is rejected");
   static_assert(!versionsCompare("1..0", "1.0.0", 0), "empty components are rejected");
   static_assert(!versionsCompare("1.0.0.1", "1.0.0", 0), "a fourth component is rejected");
   static_assert(!versionsCompare("", "1.0.0", 0), "an empty version is rejected");

   // Names the outcome for the OTA VER log line.
   const char *versionStatusName(Ota::VersionStatus status) {
      switch (status) {
         case Ota::VersionStatus::UpToDate:
            return "up_to_date";
         case Ota::VersionStatus::UpdateAvailable:
            return "update_available";
         default:
            return "unknown";
      }
   }

   // Host part of a URL, so OTA_VERSION_URL stays the single source for the endpoint.
   String urlHost(const String &url) {
      const int scheme = url.indexOf("://");
      const int start = (scheme < 0) ? 0 : (scheme + 3);
      const int end = url.indexOf('/', start);

      return (end < 0) ? url.substring(start) : url.substring(start, end);
   }

   // GitHub quota/limit response headers. Named once so collectHeaders() and header() always use
   // exactly the same string (header() matches the collected key by content).
   constexpr char HDR_RATE_LIMIT[]     = "X-RateLimit-Limit";
   constexpr char HDR_RATE_REMAINING[] = "X-RateLimit-Remaining";
   constexpr char HDR_RATE_RESET[]     = "X-RateLimit-Reset";
   constexpr char HDR_RETRY_AFTER[]    = "Retry-After";
} // namespace

bool Ota::begin() {
   return true;
}

// Compares the latest release published on GitHub (OTA_VERSION_URL, field tag_name) with
// FIRMWARE_VERSION. Returns Unknown whenever the comparison cannot be established with
// certainty, and the caller then leaves the OTA request untouched: there is deliberately
// no fallback to "install latest".
Ota::VersionStatus Ota::checkVersion(String &remoteVersion) {
   remoteVersion = "";

   if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("[%lu] OTA VER: sem Wi-Fi, versao remota nao verificada\n", static_cast<unsigned long>(millis()));
      return VersionStatus::Unknown;
   }

   const String url = OTA_VERSION_URL;
   const String host = urlHost(url);

   if (host.isEmpty()) {
      Serial.println("OTA VER: OTA_VERSION_URL invalida");
      return VersionStatus::Unknown;
   }

   // WiFiClientSecure::connect(host) does its own DNS lookup with a fixed 10 s timeout that
   // ignores setTimeout(); resolving here first bounds that wait and leaves the answer in the
   // lwIP DNS cache, so the framework lookup is answered without a second DNS (same as TG).
   IPAddress resolved;

   if (!WiFi.hostByName(host.c_str(), resolved, OTA_VERSION_CONNECT_TIMEOUT_MS)) {
      Serial.printf("[%lu] OTA VER: DNS falhou host=%s\n", static_cast<unsigned long>(millis()), host.c_str());
      return VersionStatus::Unknown;
   }

   // TLS profile identical to the Telegram/ThingsBoard clients, which is proven to fit the
   // heap during normal operation: this check runs while both of them are connected.
   WiFiClientSecure client;
   client.setInsecure();
   client.setTimeout(OTA_VERSION_TIMEOUT_MS);
   client.setBufferSizes(4096, 512);

   if (!client.connect(host.c_str(), 443)) {
      char sslErr[128] = {0};
      const int sslCode = client.getLastSSLError(sslErr, sizeof(sslErr));

      client.stop();
      Serial.printf("[%lu] OTA VER: connect falhou host=%s ssl_code=%d ssl=%s\n", static_cast<unsigned long>(millis()), host.c_str(), sslCode, sslErr);
      return VersionStatus::Unknown;
   }

   HTTPClient http;

   if (!http.begin(client, url)) {
      client.stop();
      Serial.println("OTA VER: begin falhou");
      return VersionStatus::Unknown;
   }

   // The GitHub API rejects requests without a User-Agent; the installed version travels in
   // it, which also identifies the request in the API logs.
   http.setUserAgent(String("Monitora_DG/") + FIRMWARE_VERSION);
   http.addHeader("Accept", "application/vnd.github+json");

   // Authenticate with the repository Personal Access Token already present in secrets.h. The
   // authenticated quota replaces the anonymous 60 requests/hour. The token is used ONLY here and is
   // NEVER printed: no log below includes the Authorization header or its value.
   http.addHeader("Authorization", String("Bearer ") + TELEGRAM_REPO_TOKEN);

   // GitHub sends its quota in these headers. HTTPClient discards uncollected response headers, so
   // they must be requested BEFORE the call, otherwise header() would always return an empty string.
   const char *quotaHeaders[] = {HDR_RATE_LIMIT, HDR_RATE_REMAINING, HDR_RATE_RESET, HDR_RETRY_AFTER};
   http.collectHeaders(quotaHeaders, sizeof(quotaHeaders) / sizeof(quotaHeaders[0]));

   const int status = http.GET();

   VersionStatus result = VersionStatus::Unknown;
   size_t bodyLength = 0;
   const int mfln = client.getMFLNStatus();

   // Quota diagnostics (never the token): the HTTP code plus GitHub's rate-limit headers, so a rate
   // limit can be told apart from an HTTP/DNS/timeout failure. No retry is ever attempted.
   const String headerLimit = http.header(HDR_RATE_LIMIT);
   const String headerRemaining = http.header(HDR_RATE_REMAINING);
   const String headerReset = http.header(HDR_RATE_RESET);
   const String headerRetryAfter = http.header(HDR_RETRY_AFTER);

   char resetText[24] = "-";

   if (!headerReset.isEmpty()) {
      const uint32_t resetEpoch = static_cast<uint32_t>(strtoul(headerReset.c_str(), nullptr, 10));

      if (resetEpoch > 0) {
         // Reuses the project's Brasilia formatter. It formats the absolute epoch as-is, so it does
         // not depend on the local clock being NTP-synced.
         TimeSource::formatBrasilia(resetEpoch, resetText, sizeof(resetText));
      }
   }

   Serial.printf("[%lu] OTA VER: auth=1 http=%d ratelimit=%s/%s reset=%s retry_after=%s\n", static_cast<unsigned long>(millis()), status,
                 headerLimit.isEmpty() ? "-" : headerLimit.c_str(), headerRemaining.isEmpty() ? "-" : headerRemaining.c_str(), resetText,
                 headerRetryAfter.isEmpty() ? "-" : headerRetryAfter.c_str());

   if (status == HTTP_CODE_OK) {
      // getString() returns the payload owned by HTTPClient (valid until end()), so holding a
      // reference avoids duplicating the ~3.3 kB release JSON. Content-Length and chunked
      // framing are both decoded by the core, so GitHub may answer either way.
      const String &body = http.getString();

      bodyLength = body.length();

      if (bodyLength == 0 || bodyLength > OTA_VERSION_BODY_MAX) {
         Serial.printf("OTA VER: corpo inesperado (%u bytes)\n", static_cast<unsigned>(bodyLength));
      } else {
         // The filter keeps tag_name only: the release notes and the assets array are skipped
         // while parsing, so the document stays tiny instead of holding the whole JSON.
         DynamicJsonDocument filter(256);
         filter["tag_name"] = true;

         DynamicJsonDocument doc(256);
         const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));

         if (err) {
            Serial.printf("OTA VER: json invalido (%s)\n", err.c_str());
         } else {
            const char *tag = doc["tag_name"];

            Version installed = {{0, 0, 0}};
            Version remote = {{0, 0, 0}};

            if (tag == nullptr || tag[0] == '\0') {
               Serial.println("OTA VER: tag_name ausente ou vazio");
            } else if (!parseVersion(FIRMWARE_VERSION, installed)) {
               // Firmware configuration error, never a network one: do not update on top of it.
               Serial.printf("OTA VER: FIRMWARE_VERSION invalida (\"%s\")\n", FIRMWARE_VERSION);
            } else if (!parseVersion(tag, remote)) {
               Serial.printf("OTA VER: tag_name invalido (\"%.32s\")\n", tag);
            } else {
               remoteVersion = String(remote.parts[0]) + "." + remote.parts[1] + "." + remote.parts[2];
               result = (compareVersions(remote, installed) > 0) ? VersionStatus::UpdateAvailable : VersionStatus::UpToDate;
            }
         }
      }
   } else if (status == 403 || status == 429) {
      // GitHub's primary (403) and secondary (429) rate limits. Nothing is retried: the /ota request
      // is reported as not verified and abandoned until the user sends /ota again.
      Serial.printf("[%lu] OTA VER: rate limit do GitHub http=%d; sem retry, versao remota nao verificada\n",
                    static_cast<unsigned long>(millis()), status);
   } else if (status > 0) {
      // Any other definitive HTTP reply (401 token invalido, 404, 5xx, ...).
      Serial.printf("[%lu] OTA VER: resposta HTTP inesperada http=%d; versao remota nao verificada\n", static_cast<unsigned long>(millis()), status);
   } else {
      // Negative code: no HTTP reply at all (connect/TLS failure; a read timeout is
      // HTTPC_ERROR_READ_TIMEOUT = -11).
      Serial.printf("[%lu] OTA VER: falha de transporte http=%d (connect/TLS/timeout); versao remota nao verificada\n",
                    static_cast<unsigned long>(millis()), status);
   }

   http.end();
   client.stop();

   Serial.printf("[%lu] OTA VER: http=%d body=%u mfln=%d free=%u maxblock=%u instalada=%s remota=%s resultado=%s\n", static_cast<unsigned long>(millis()), status,
                 static_cast<unsigned>(bodyLength), mfln, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxFreeBlockSize()),
                 FIRMWARE_VERSION, remoteVersion.isEmpty() ? "-" : remoteVersion.c_str(), versionStatusName(result));

   return result;
}

bool Ota::run() {
   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   WiFiClientSecure client;
   client.setInsecure();

   // GitHub does not serve the asset directly: /releases/latest/download/... answers 302
   // twice (github.com -> .../download/<tag>/... -> release-assets.githubusercontent.com)
   // and ESP8266HTTPUpdate disables redirect following by default, so the update could
   // never start. STRICT is enough: the redirects are 302 and the method is GET.
   ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

   // This client keeps the BearSSL default buffers: 16384 + 325 = 16709 B of CONTIGUOUS
   // RAM (~16.7 kB). Shrinking them with MFLN was tried and failed: the MFLN probe only
   // covers the URL host (github.com), while GitHub answers 302 and the body is served by
   // release-assets.githubusercontent.com, which does not negotiate MFLN - so a smaller
   // buffer ended with BR_ERR_TOO_LARGE (incoming record larger than the buffer) after
   // ~44 kB. main.cpp therefore runs ota.run() from runPendingOtaIfRequested() on a clean
   // boot, before Telegram and ThingsBoard exist, so the default buffer fits.
   //
   // Operational diagnostic: heap right before the OTA client connects.
   Serial.printf("OTA HEAP: free=%u maxblock=%u\n", ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());

   t_httpUpdate_return result =
      ESPhttpUpdate.update(client, OTA_FIRMWARE_URL);

   switch (result) {
      case HTTP_UPDATE_FAILED:
         Serial.printf(
            "OTA falhou: (%d) %s\n",
            ESPhttpUpdate.getLastError(),
            ESPhttpUpdate.getLastErrorString().c_str()
         );
         return false;

      case HTTP_UPDATE_NO_UPDATES:
         Serial.println("OTA: nenhuma atualização disponível.");
         return false;

      case HTTP_UPDATE_OK:
         // Normally does not return: ESP8266HTTPUpdate reboots after success.
         Serial.println("OTA concluída.");
         return true;
   }

   return false;
}
