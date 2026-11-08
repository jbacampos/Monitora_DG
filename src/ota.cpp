#include "ota.h"

#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <umm_malloc/umm_heap_select.h>

#include "config.h"
#include "heap_diag.h"
#include "ota_asset.h"
#include "secrets.h"
#include "time_source.h"
#include "types.h"

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

   // Resolves the STABLE API URL of the release asset named OTA_ASSET_NAME from the /releases/latest
   // `assets` array into `assetUrl`. Leaves it EMPTY when the asset is absent, ambiguous, has no URL
   // or the URL does not fit. The exact/unique-match rule lives in the pure selectAssetUrl()
   // (ota_asset.h), covered by the host tests.
   void resolveAssetUrl(JsonArray assets, String &assetUrl) {
      constexpr size_t kMaxAssets = 8;
      const char *names[kMaxAssets];
      const char *urls[kMaxAssets];
      size_t count = 0;

      for (JsonObject asset : assets) {
         if (count >= kMaxAssets) {
            break;
         }

         names[count] = asset["name"].as<const char *>();
         urls[count] = asset["url"].as<const char *>();
         ++count;
      }

      char buffer[OTA_ASSET_URL_MAX];

      if (selectAssetUrl(names, urls, count, OTA_ASSET_NAME, buffer, sizeof(buffer))) {
         assetUrl = buffer;
      }
   }

   // Complementary OTA heap guard threshold (see run()): below this largest free block the TLS
   // handshake's THROWING allocations (br_ssl_client_context ~3424 B + br_x509_insecure_context
   // ~1496 B) are at risk and this core would panic instead of failing gracefully.
   constexpr uint32_t OTA_TLS_MIN_BLOCK = 4096;

   // Diagnostic-only: free heap + largest block for the CURRENT heap and for DRAM and IRAM
   // SEPARATELY. HeapSelect{...} swaps the allocator's current heap and restores it on scope exit
   // (RAII), so nothing leaks; no allocation is attempted (pure read).
   void logOtaMem(const char *label) {
      const uint32_t freeCur = ESP.getFreeHeap();
      const uint32_t blockCur = ESP.getMaxFreeBlockSize();

      uint32_t freeDram = 0;
      uint32_t blockDram = 0;
      uint32_t freeIram = 0;
      uint32_t blockIram = 0;

      {
         HeapSelectDram dram;
         freeDram = ESP.getFreeHeap();
         blockDram = ESP.getMaxFreeBlockSize();
      }

      {
         HeapSelectIram iram;
         freeIram = ESP.getFreeHeap();
         blockIram = ESP.getMaxFreeBlockSize();
      }

      Serial.printf("[%lu] OTA MEM %s: cur=%u/%u dram=%u/%u iram=%u/%u (free/maxblock)\n", static_cast<unsigned long>(millis()), label, freeCur, blockCur,
                    freeDram, blockDram, freeIram, blockIram);
   }

   // Diagnostic-only: shared state for the ESPhttpUpdate/Updater callbacks registered in run(). The
   // client pointer is set immediately BEFORE update() and cleared immediately AFTER it returns, so a
   // callback can never dereference a destroyed client and no dangling pointer is left behind.
   WiFiClientSecure *g_otaClient = nullptr;
   uint32_t g_otaStartMs = 0;
   uint32_t g_otaLastLogged = 0;
} // namespace

bool Ota::begin() {
   return true;
}

// Compares the latest release published on GitHub (OTA_VERSION_URL, field tag_name) with
// FIRMWARE_VERSION. Returns Unknown whenever the comparison cannot be established with
// certainty, and the caller then leaves the OTA request untouched: there is deliberately
// no fallback to "install latest".
Ota::VersionStatus Ota::checkVersion(String &remoteVersion, String &assetUrl) {
   remoteVersion = "";
   assetUrl = "";

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
   stackThunkDiag("ota:ver:pre-client"); // (2) antes do construtor do cliente TLS da OTA

   WiFiClientSecure client;

   stackThunkDiag("ota:ver:pos-client"); // (2) depois do construtor
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
         // The filter keeps only the fields the check needs: tag_name and each asset's name/url. The
         // release notes, the asset bodies and everything else are skipped while parsing, so the
         // document stays tiny instead of holding the whole JSON.
         DynamicJsonDocument filter(512);
         filter["tag_name"] = true;
         filter["assets"][0]["name"] = true;
         filter["assets"][0]["url"] = true;

         DynamicJsonDocument doc(1024);
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
               const bool newer = compareVersions(remote, installed) > 0;
               result = newer ? VersionStatus::UpdateAvailable : VersionStatus::UpToDate;

               if (newer) {
                  // Resolve the STABLE asset endpoint NOW, from the SAME query. An empty assetUrl
                  // means the release has no Monitora_DG.bin asset (or it is ambiguous): the caller
                  // then aborts the update explicitly.
                  resolveAssetUrl(doc["assets"].as<JsonArray>(), assetUrl);
               }
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

   Serial.printf("[%lu] OTA VER: http=%d body=%u mfln=%d free=%u maxblock=%u instalada=%s remota=%s resultado=%s asset=%s\n",
                 static_cast<unsigned long>(millis()), status, static_cast<unsigned>(bodyLength), mfln, static_cast<unsigned>(ESP.getFreeHeap()),
                 static_cast<unsigned>(ESP.getMaxFreeBlockSize()), FIRMWARE_VERSION, remoteVersion.isEmpty() ? "-" : remoteVersion.c_str(),
                 versionStatusName(result), assetUrl.isEmpty() ? "-" : assetUrl.c_str());

   return result;
}

bool Ota::run(const char *assetUrl) {
   stackThunkDiag("ota:run:start"); // (3) inicio de Ota::run()

   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   if (assetUrl == nullptr || assetUrl[0] == '\0') {
      // Never fall back to the previous download URL: without the persisted asset endpoint there is
      // nothing safe to download.
      Serial.println("OTA: endpoint do asset ausente; atualizacao nao iniciada.");
      return false;
   }

   logOtaMem("pre-cliente"); // (1) antes de criar o cliente TLS

   stackThunkDiag("ota:pre-cliente"); // (4) imediatamente antes do construtor do cliente TLS da OTA

   WiFiClientSecure client;

   stackThunkDiag("ota:pos-cliente"); // (4) imediatamente depois do construtor
   client.setInsecure();

   // Reduce ONLY the OTA client's BearSSL buffers. The default RX (16384 data + 325 overhead = 16709 B)
   // is the dominant heap consumer of the TLS handshake and, when it does not fit in IRAM, falls to
   // DRAM. 13312 (13637 B with overhead) lowers that peak while staying close to the TLS record
   // maximum (the CDN does not negotiate MFLN). Telegram and ThingsBoard keep their own clients and
   // buffer sizes untouched.
   client.setBufferSizes(13312, 512);

   logOtaMem("pos-buffers"); // (2) depois de configurar os buffers

   HTTPClient http;

   // begin() must precede addHeader(): begin() -> beginInternal() -> clear() wipes the request headers.
   if (!http.begin(client, String(assetUrl))) {
      client.stop();
      Serial.println("OTA: http.begin falhou (endpoint do asset invalido)");
      return false;
   }

   // The asset API endpoint answers the BINARY (302 -> CDN) only with this header; without it the API
   // returns the asset JSON metadata (HTTP 200) and the update would fail with a wrong HTTP code.
   // handleUpdate() appends its own x-ESP8266-* headers but never clears this one, and the 302 is
   // followed by HTTPClient in STRICT mode. NO Authorization is added: the PAT must never travel to
   // github.com or to the CDN.
   http.addHeader("Accept", "application/octet-stream");

   // GitHub's asset endpoint answers 302 to release-assets.githubusercontent.com; ESP8266HTTPUpdate
   // disables redirect following by default, so STRICT is required (the redirects are 302 and the
   // method is GET).
   ESPhttpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

   logOtaMem("pre-update"); // (3) imediatamente antes de ESPhttpUpdate.update()

   stackThunkDiag("ota:pre-update"); // (5) imediatamente antes de ESPhttpUpdate.update()

   // Complementary heap guard (NOT a guarantee against later allocation failures): the BearSSL
   // handshake performs THROWING C++ allocations (make_shared<br_ssl_client_context> ~3424 B and
   // <br_x509_insecure_context> ~1496 B). This core's operator new calls __unhandled_exception("OOM")
   // directly (abi.cpp; the build uses -fno-exceptions), so an out-of-memory there is a PANIC and can
   // NOT be caught. If the largest free block cannot even fit the SSL context, abort the attempt
   // gracefully instead of risking the panic.
   const uint32_t maxBlock = ESP.getMaxFreeBlockSize();
   if (maxBlock < OTA_TLS_MIN_BLOCK) {
      Serial.printf("[%lu] OTA: heap insuficiente (maxblock=%u < %u); atualizacao nao iniciada\n", static_cast<unsigned long>(millis()),
                    static_cast<unsigned>(maxBlock), static_cast<unsigned>(OTA_TLS_MIN_BLOCK));
      http.end();
      client.stop();
      return false;
   }

   // --- Diagnostic instrumentation (read-only observers; they NEVER change the transfer) -----------
   // The OTA client pointer is valid ONLY between here and the clear after update() returns. The host
   // is logged (no tokens, no signed URLs) for the current step.
   Serial.printf("[%lu] OTA: download host=%s\n", static_cast<unsigned long>(millis()), urlHost(String(assetUrl)).c_str());

   g_otaClient = &client;
   g_otaStartMs = millis();
   g_otaLastLogged = 0;

   // onStart fires once the FINAL response was HTTP 200 with Content-Length > 0 and enough flash
   // space (i.e. the API 302 was followed and the CDN answered). The total size is NOT known here
   // (Update.begin has not run yet): it is reported by the first onProgress(0, total).
   ESPhttpUpdate.onStart([]() {
      Serial.printf("[%lu] OTA: corpo iniciado (HTTP 200 aceito); mfln=%d\n", static_cast<unsigned long>(millis() - g_otaStartMs),
                    g_otaClient ? g_otaClient->getMFLNStatus() : -1);
   });

   // onProgress: `current` = bytes already written to flash (= received); `total` = Content-Length.
   // Logged at the start, every ~16 KB and on the final sample, bounding the log volume.
   ESPhttpUpdate.onProgress([](int current, int total) {
      if (current == 0 || current >= static_cast<int>(g_otaLastLogged + 16384u) || current == total) {
         Serial.printf("[%lu] OTA: progresso %d/%d bytes\n", static_cast<unsigned long>(millis() - g_otaStartMs), current, total);
         g_otaLastLogged = static_cast<uint32_t>(current);
      }
   });

   ESPhttpUpdate.onEnd([]() {
      Serial.printf("[%lu] OTA: transferencia concluida (%u bytes)\n", static_cast<unsigned long>(millis() - g_otaStartMs),
                    static_cast<unsigned>(Update.progress()));
   });

   // Registered on the Updater (NOT ESPhttpUpdate): this fires the instant writeStream() sets the
   // stream error, while the TLS client and its BearSSL engine are still alive, so getLastSSLError()
   // is valid (it reads _sc/_eng, which http.end() frees). sslCode == 0 means NO TLS error was found.
   Update.onError([](int err) {
      char sslMsg[96] = {0};
      const int sslCode = g_otaClient ? g_otaClient->getLastSSLError(sslMsg, sizeof(sslMsg)) : 0;
      Serial.printf("[%lu] OTA: update_error=%d progresso=%u bytes\n", static_cast<unsigned long>(millis() - g_otaStartMs), err,
                    static_cast<unsigned>(Update.progress()));
      if (sslCode != 0) {
         Serial.printf("[%lu] OTA: BearSSL ERRO codigo=%d \"%s\"\n", static_cast<unsigned long>(millis() - g_otaStartMs), sslCode, sslMsg);
      } else {
         Serial.printf("[%lu] OTA: BearSSL sem erro TLS (codigo 0)\n", static_cast<unsigned long>(millis() - g_otaStartMs));
      }
   });
   // --- end diagnostic instrumentation --------------------------------------------------------------

   // handleUpdate() (called by update()) ends the HTTPClient on EVERY return path, so the socket is
   // always closed; the client we passed keeps its TLS profile (insecure, reduced buffers).
   const t_httpUpdate_return result = ESPhttpUpdate.update(http);

   // Defensive and idempotent: end() on an already-ended client only stops the socket again, and
   // client.stop() is a no-op once stopped. Guarantees closure on every execution path.
   http.end();
   client.stop();

   g_otaClient = nullptr; // never leave the callback pointer dangling after the operation

   logOtaMem("pos-update"); // (4) depois do retorno da atualizacao (sucesso ou falha)

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
