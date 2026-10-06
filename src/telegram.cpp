#include "telegram.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "secrets.h"

namespace {
   constexpr char TELEGRAM_HOST[] = "api.telegram.org";

   constexpr size_t TG_DIAG_BODY_MAX = 160;
   constexpr size_t TG_DIAG_DESC_MAX = 120;

   // Diagnostic-only: elapsed milliseconds since "start".
   uint32_t elapsedMs(uint32_t start) {
      return static_cast<uint32_t>(millis() - start);
   }

   // Diagnostic-only: bounded, single-line view of a raw HTTP body snippet.
   // CR/LF become spaces so the whole snippet stays on one Serial line.
   String diagBody(const String &body) {
      String out = body.substring(0, TG_DIAG_BODY_MAX);
      out.replace("\r", " ");
      out.replace("\n", " ");
      return out;
   }

   // Diagnostic-only: clock + duration wrapper for the per-request summary.
   void httpLog(const char *method, bool ok, int http, uint32_t start) {
      Serial.printf("[%lu] TG HTTP: %s fim ok=%d http=%d duracao=%lums\n", static_cast<unsigned long>(millis()), method, ok ? 1 : 0, http,
                    static_cast<unsigned long>(elapsedMs(start)));
   }

   // Diagnostic-only: the request failed before any HTTP reply was read, so the
   // transport state is made explicit.
   void httpTransportLog(const char *method, const char *state, uint32_t start) {
      Serial.printf("[%lu] TG HTTP: %s transporte=%s http=0 duracao=%lums\n", static_cast<unsigned long>(millis()), method, state,
                    static_cast<unsigned long>(elapsedMs(start)));
   }

   // Diagnostic-only: heap snapshot around the TLS client lifecycle.
   // free = ESP.getFreeHeap(), maxblock = ESP.getMaxFreeBlockSize(),
   // frag = ESP.getHeapFragmentation() in %.
   void heapLog(const char *method, const char *tag) {
      Serial.printf("[%lu] TG HEAP: %s %s free=%u maxblock=%u frag=%u%%\n", static_cast<unsigned long>(millis()), method, tag,
                    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxFreeBlockSize()),
                    static_cast<unsigned>(ESP.getHeapFragmentation()));
   }

   // ---------------------------------------------------------------------------
   // Persistent Telegram TLS connection.
   //
   // Created on demand, reused by every Telegram request (getUpdates /
   // deleteMessage / sendSticker / sendMessage) and closed explicitly by
   // Telegram::closeConnection(). Requests use HTTP/1.1 keep-alive, so the end of
   // each response is determined by its own framing (Content-Length or chunked)
   // and never by the socket closing.
   // ---------------------------------------------------------------------------
   WiFiClientSecure *tgClient = nullptr;

   // Instrumentation only: it measures
   // the effect of spacing reused requests and never changes the HTTP/TLS logic.
   uint32_t tgLastReqEndMs = 0; // millis() when the previous request really ended
   bool tgReqReused = false;    // the request in flight uses a reused connection
   bool tgReqAnyByte = false;   // at least one response byte was received
   bool tgReqWriteOk = false;   // the request write completed normally

   uint32_t tgReuseAttempts = 0;  // reuse branch taken in ensureConnection()
   uint32_t tgReuseSuccess = 0;   // reused request whose response was framed
   uint32_t tgReuseStalls = 0;    // reused + write ok + zero bytes + NO_DATA_TIMEOUT
   uint32_t tgNewConnections = 0; // TLS connections actually established

   // Phase timestamps of the response read in flight, all
   // compared against tgT0 (the instant the request write returned). 0 means the
   // phase was never reached.
   uint32_t tgT0 = 0; // right after the request write returned
   uint32_t tgT1 = 0; // first response byte
   uint32_t tgT2 = 0; // status line complete
   uint32_t tgT3 = 0; // headers complete
   uint32_t tgT4 = 0; // body complete (success only)

   // Prints a phase as "<n>ms" relative to t0, or "NONE".
   void tgPhaseText(char *out, size_t len, uint32_t stamp, uint32_t t0) {
      if (stamp == 0) {
         snprintf(out, len, "NONE");
      } else {
         snprintf(out, len, "%lums", static_cast<unsigned long>(stamp - t0));
      }
   }

   // Diagnostic-only: connection lifecycle of the persistent TLS client.
   void tlsLog(const char *state) {
      Serial.printf("[%lu] TG TLS: %s\n", static_cast<unsigned long>(millis()), state);
   }

   // Stops and releases the persistent client (safe when none exists).
   void dropTgConnection() {
      if (tgClient != nullptr) {
         tgClient->stop();
         delete tgClient;
         tgClient = nullptr;
         tlsLog("conexao fechada");
      }
   }

   // Overflow-safe absolute-deadline test.
   bool tgExpired(uint32_t deadline) {
      return static_cast<int32_t>(millis() - deadline) >= 0;
   }

   // Creates the persistent client when missing/dead, otherwise reuses it.
   bool ensureConnection() {
      if (tgClient != nullptr && tgClient->connected()) {
         tlsLog("conexao reutilizada");
         tgReuseAttempts++;
         tgReqReused = true;
         return true;
      }

      if (tgClient != nullptr) {
         // The server (or the network) closed the previous connection.
         tlsLog("reconectando");
         dropTgConnection();
      }

      WiFiClientSecure *client = new WiFiClientSecure();

      client->setInsecure();
      client->setTimeout(TELEGRAM_RESPONSE_TIMEOUT_MS);
      client->setBufferSizes(4096, 512);

      // WiFiClientSecureCtx::connect(host) does its own DNS lookup with a fixed
      // 10 s timeout that ignores setTimeout(); resolving here first bounds that
      // wait and leaves the answer in the lwIP DNS cache, so the framework lookup
      // answers "address already known" (ERR_OK) without a second DNS.
      IPAddress resolved;

      if (!WiFi.hostByName(TELEGRAM_HOST, resolved, TELEGRAM_CONNECT_TIMEOUT_MS)) {
         client->stop();
         delete client;
         return false;
      }

      if (!client->connect(TELEGRAM_HOST, 443)) {
         client->stop();
         delete client;
         return false;
      }

      tgClient = client;
      tgNewConnections++;
      tgReqReused = false;
      tlsLog("novo cliente");
      return true;
   }

   // Reads one byte, waiting at most until `deadline`. -1 on timeout/disconnect.
   int readByte(uint32_t deadline) {
      while (!tgExpired(deadline)) {
         if (tgClient->available()) {
            const int c = tgClient->read();

            if (c >= 0) {
               if (!tgReqAnyByte) {
                  tgReqAnyByte = true;
                  tgT1 = millis(); // first response byte
               }

               return c;
            }
         } else if (!tgClient->connected()) {
            return -1;
         }

         yield();
      }

      return -1;
   }

   // Reads one CRLF terminated line (CRLF stripped). false on timeout/disconnect.
   bool readLine(uint32_t deadline, String &line) {
      line = "";

      for (;;) {
         const int c = readByte(deadline);

         if (c < 0) {
            return false;
         }

         if (c == '\n') {
            return true;
         }

         if (c != '\r') {
            if (line.length() >= 512) {
               return false; // overlong/malformed header line
            }

            line += static_cast<char>(c);
         }
      }
   }

   // Appends exactly `length` body bytes to `out`.
   bool readBodyInto(uint32_t deadline, size_t length, String &out) {
      for (size_t i = 0; i < length; ++i) {
         const int c = readByte(deadline);

         if (c < 0) {
            return false;
         }

         out += static_cast<char>(c);
      }

      return true;
   }

   // Reads exactly one HTTP/1.1 response (status line + headers + body) from the
   // persistent client. Content-Length and Transfer-Encoding: chunked are
   // honoured and the reader never consumes bytes belonging to the next response,
   // so the connection stays reusable. false on any framing/transport error.
   bool readHttpResponse(uint32_t deadline, String &body, int &status) {
      String line;

      if (!readLine(deadline, line) || line.indexOf("HTTP/") != 0) {
         return false;
      }

      tgT2 = millis(); // status line complete

      status = line.substring(line.indexOf(' ') + 1).toInt();

      bool chunked = false;
      long contentLength = -1;

      // Diagnostic-only: keep-alive relevant headers exactly as the server sent
      // them, so the connection policy can be read from the device itself.
      String connectionHeader;
      String transferEncodingHeader;

      for (;;) {
         if (!readLine(deadline, line)) {
            return false;
         }

         if (line.isEmpty()) {
            break;
         }

         const int colon = line.indexOf(':');

         if (colon <= 0) {
            continue; // malformed header: ignore it
         }

         String name = line.substring(0, colon);
         name.toLowerCase();

         String value = line.substring(colon + 1);
         value.trim();

         if (name == "content-length") {
            contentLength = value.toInt();
         } else if (name == "connection") {
            connectionHeader = value;
         } else if (name == "transfer-encoding") {
            transferEncodingHeader = value;
            value.toLowerCase();

            if (value.indexOf("chunked") >= 0) {
               chunked = true;
            }
         }
      }

      tgT3 = millis(); // headers complete

      // Diagnostic-only: what the server declared about reusing the connection and
      // about the body framing ("-" means the header was absent).
      Serial.printf("[%lu] TG HDR: connection=\"%s\" content_length=%ld transfer_encoding=\"%s\"\n", static_cast<unsigned long>(millis()),
                    connectionHeader.isEmpty() ? "-" : connectionHeader.c_str(), contentLength,
                    transferEncodingHeader.isEmpty() ? "-" : transferEncodingHeader.c_str());

      body = "";
      body.reserve(1024);

      if (chunked) {
         for (;;) {
            if (!readLine(deadline, line)) {
               return false;
            }

            const int semi = line.indexOf(';');

            if (semi >= 0) {
               line = line.substring(0, semi); // drop the chunk extension
            }

            line.trim();

            const long size = strtol(line.c_str(), nullptr, 16);

            if (size < 0) {
               return false;
            }

            if (size == 0) {
               // Consume the trailers up to the closing empty line.
               for (;;) {
                  if (!readLine(deadline, line)) {
                     return false;
                  }

                  if (line.isEmpty()) {
                     break;
                  }
               }

               break;
            }

            if (!readBodyInto(deadline, static_cast<size_t>(size), body)) {
               return false;
            }

            // Each chunk's data is followed by its own CRLF.
            if (!readLine(deadline, line) || !line.isEmpty()) {
               return false;
            }
         }

         return true;
      }

      if (contentLength < 0) {
         return false; // no length and no chunking: cannot frame a keep-alive reply
      }

      return readBodyInto(deadline, static_cast<size_t>(contentLength), body);
   }

   // Diagnostic-only: what Telegram actually answered. Prints the parsed
   // api ok / error_code / description, a bounded body snippet, or a compact
   // line for large replies (getUpdates). Never prints the token or the URL.
   void httpResponseLog(const char *method, const String &response, int http, uint32_t start) {
      const uint32_t dur = elapsedMs(start);

      // getUpdates replies can be large; keep them to a compact line only.
      if (String(method) == "getUpdates") {
         Serial.printf("[%lu] TG HTTP: %s transporte=OK http=%d duracao=%lums\n", static_cast<unsigned long>(millis()), method, http,
                       static_cast<unsigned long>(dur));
         return;
      }

      if (response.isEmpty()) {
         Serial.printf("[%lu] TG HTTP: %s transporte=OK http=%d body=<vazio> duracao=%lums\n", static_cast<unsigned long>(millis()), method, http,
                       static_cast<unsigned long>(dur));
         return;
      }

      const int jsonStart = response.indexOf("{\"ok\"");

      if (jsonStart < 0) {
         Serial.printf("[%lu] TG HTTP: %s transporte=OK http=%d api_ok=? body=\"%s\" duracao=%lums\n", static_cast<unsigned long>(millis()), method, http,
                       diagBody(response).c_str(), static_cast<unsigned long>(dur));
         return;
      }

      DynamicJsonDocument doc(2048);

      const DeserializationError err = deserializeJson(doc, response.substring(jsonStart));

      if (err) {
         Serial.printf("[%lu] TG HTTP: %s http=%d api_ok=? json_erro=%s body=\"%s\" duracao=%lums\n", static_cast<unsigned long>(millis()), method, http, err.c_str(),
                       diagBody(response).c_str(), static_cast<unsigned long>(dur));
         return;
      }

      const bool apiOk = doc["ok"] | false;

      if (apiOk) {
         Serial.printf("[%lu] TG HTTP: %s transporte=OK http=%d api_ok=1 duracao=%lums\n", static_cast<unsigned long>(millis()), method, http,
                       static_cast<unsigned long>(dur));
         return;
      }

      const int errCode = doc["error_code"] | 0;
      String desc = doc["description"] | "";

      desc.replace("\r", " ");
      desc.replace("\n", " ");

      Serial.printf("[%lu] TG HTTP: %s transporte=OK http=%d api_ok=0 error_code=%d description=\"%s\" duracao=%lums\n", static_cast<unsigned long>(millis()), method,
                    http, errCode, desc.substring(0, TG_DIAG_DESC_MAX).c_str(), static_cast<unsigned long>(dur));
   }
} // namespace

bool Telegram::begin(int32_t restoredUpdateOffset) {
   lastPoll_ = 0;
   updateOffset_ = restoredUpdateOffset;
   // With a restored offset there is no stale backlog to skip: the first poll must ask
   // for offset = updateOffset_. Otherwise Telegram returns the last update (the very
   // /reboot that restarted the device) and the command would loop forever.
   firstPoll_ = (updateOffset_ == 0);
   rebootRequested_ = false;
   otaRequested_ = false;
   return true;
}

void Telegram::closeConnection() {
   dropTgConnection();
}

// Diagnostic counters for the periodic DIAG line.
uint32_t Telegram::reuseAttempts() const {
   return tgReuseAttempts;
}

uint32_t Telegram::reuseSuccess() const {
   return tgReuseSuccess;
}

uint32_t Telegram::reuseStalls() const {
   return tgReuseStalls;
}

uint32_t Telegram::newConnections() const {
   return tgNewConnections;
}

String Telegram::urlEncode(const String &value) {
   String encoded;
   encoded.reserve(value.length() * 3);

   const char *hex = "0123456789ABCDEF";

   for (size_t i = 0; i < value.length(); ++i) {
      const uint8_t c = static_cast<uint8_t>(value[i]);

      if ((c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == '~') {
         encoded += static_cast<char>(c);
      } else {
         encoded += '%';
         encoded += hex[(c >> 4) & 0x0F];
         encoded += hex[c & 0x0F];
      }
   }

   return encoded;
}

bool Telegram::request(
   const String &method,
   const String &query,
   String &response,
   bool allowReuse) {

   const uint32_t httpStart = millis();

   Serial.printf("[%lu] TG HTTP: %s inicio\n", static_cast<unsigned long>(httpStart), method.c_str());

   heapLog(method.c_str(), "pre-new");

   // getUpdates() deliberately never reuses the persistent connection.
   // Any connection left over from a previous operation is closed here, so the
   // ensureConnection() below always opens a fresh TLS connection.
   if (!allowReuse) {
      Serial.printf("[%lu] TG TLS: %s sem reuse\n", static_cast<unsigned long>(millis()), method.c_str());

      dropTgConnection();
   }

   // Per-request instrumentation state.
   tgReqAnyByte = false;
   tgReqWriteOk = false;

   if (WiFi.status() != WL_CONNECTED) {
      dropTgConnection();
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "NO_WIFI", httpStart);
      tgLastReqEndMs = millis();
      return false;
   }

   if (!ensureConnection()) {
      heapLog(method.c_str(), "pos-connect-fail");
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "CONNECT_FAIL", httpStart);
      tgLastReqEndMs = millis();
      return false;
   }

   heapLog(method.c_str(), "pre-connect");

   // A REUSED connection is never used sooner than
   // TELEGRAM_REUSE_MIN_GAP_MS after the previous request ended. Opening a new
   // connection is not delayed. The wait is logged only when it really happens.
   if (tgReqReused) {
      const uint32_t sinceEnd = static_cast<uint32_t>(millis() - tgLastReqEndMs);

      if (sinceEnd < TELEGRAM_REUSE_MIN_GAP_MS) {
         const uint32_t waitMs = TELEGRAM_REUSE_MIN_GAP_MS - sinceEnd;

         Serial.printf("[%lu] TG TLS: reuse aguardou %lums\n", static_cast<unsigned long>(millis()), static_cast<unsigned long>(waitMs));

         delay(waitMs);
      }
   }

   const String path =
      "/bot" + String(TELEGRAM_BOT_TOKEN) +
      "/" + method +
      (query.isEmpty() ? "" : "?" + query);

   const String header =
      String("GET ") + path + " HTTP/1.1\r\n"
      "Host: " + TELEGRAM_HOST + "\r\n"
      "Connection: keep-alive\r\n\r\n";

   const size_t written = tgClient->print(header);

   // T0 = instant the request write returned. Every phase
   // timestamp of this read is compared against it.
   tgT0 = millis();
   tgT1 = 0;
   tgT2 = 0;
   tgT3 = 0;
   tgT4 = 0;

   // The write counts as normal only when the whole request was
   // accepted by the TLS layer (identical bytes, just measured).
   tgReqWriteOk = (written == header.length());

   // Diagnostic-only: what the TLS layer accepted and what the socket reports
   // immediately after the request was written. Observing only.
   Serial.printf("[%lu] TG DIAG: %s write_ret=%u avail=%d connected=%d\n", static_cast<unsigned long>(millis()), method.c_str(),
                 static_cast<unsigned>(written), tgClient->available(), tgClient->connected() ? 1 : 0);

   const uint32_t deadline = millis() + TELEGRAM_RESPONSE_TIMEOUT_MS;

   int status = 0;

   const bool framed = readHttpResponse(deadline, response, status);

   const uint32_t tReadDone = millis();

   if (framed) {
      tgT4 = tReadDone; // body complete
   }

   // One line carrying every phase, relative to T0, printed
   // for both outcomes, so a reply that arrives late is separated from a reply that
   // never arrives. Nothing about the request flow changes.
   char phFirst[16];
   char phStatus[16];
   char phHeaders[16];
   char phBody[16];

   tgPhaseText(phFirst, sizeof(phFirst), tgT1, tgT0);
   tgPhaseText(phStatus, sizeof(phStatus), tgT2, tgT0);
   tgPhaseText(phHeaders, sizeof(phHeaders), tgT3, tgT0);
   tgPhaseText(phBody, sizeof(phBody), tgT4, tgT0);

   // Diagnostic-only: the BearSSL error is read here, before any dropTgConnection()
   // below destroys the engine that holds it.
   char sslErr[160] = {0};
   const int sslCode = tgClient->getLastSSLError(sslErr, sizeof(sslErr));

   Serial.printf("[%lu] TG HTTP: %s write_ret=%u first_byte=%s status_line=%s headers=%s body=%s since_t0=%lums timeout=%lums duracao=%lums "
                 "avail=%d connected=%d ssl_code=%d rssi=%d result=%s\n",
                 static_cast<unsigned long>(millis()), method.c_str(), static_cast<unsigned>(written), phFirst, phStatus, phHeaders, phBody,
                 static_cast<unsigned long>(tReadDone - tgT0), static_cast<unsigned long>(TELEGRAM_RESPONSE_TIMEOUT_MS),
                 static_cast<unsigned long>(elapsedMs(httpStart)), tgClient->available(), tgClient->connected() ? 1 : 0, sslCode, WiFi.RSSI(),
                 framed ? "OK" : (tgReqAnyByte ? "PARTIAL_TIMEOUT" : "NO_DATA_TIMEOUT"));

   if (!framed) {
      Serial.printf("[%lu] TG DIAG: %s read_fail ssl_code=%d ssl=%s\n", static_cast<unsigned long>(millis()), method.c_str(), sslCode, sslErr);

      // stall = reused connection + normal write + zero response bytes
      // + NO_DATA_TIMEOUT. Failures on a new connection, or failures that received
      // at least one byte, are not counted.
      if (tgReqReused && tgReqWriteOk && !tgReqAnyByte) {
         tgReuseStalls++;
      }

      // The reply could not be framed, so the connection state is unknown: drop
      // it and let the existing retry behaviour take over.
      dropTgConnection();
      heapLog(method.c_str(), "pos-close-nodata");
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "NO_DATA_TIMEOUT", httpStart);
      tgLastReqEndMs = millis();
      return false;
   }

   // A reused request that produced a framed HTTP response counts as a
   // successful reuse, independently of the API level ok flag.
   if (tgReqReused) {
      tgReuseSuccess++;
   }

   // `response` now holds the body only, so the previous JSON based verdict is
   // unchanged; the connection stays open for the next operation.
   const bool ok = response.indexOf("{\"ok\"") >= 0;

   httpLog(method.c_str(), ok, status, httpStart);
   httpResponseLog(method.c_str(), response, status, httpStart);

   heapLog(method.c_str(), "pos-resposta");

   tgLastReqEndMs = millis();

   // A connection opened for getUpdates() is not left behind for the
   // next request; every other method keeps the persistent connection.
   if (!allowReuse) {
      dropTgConnection();
   }

   return ok;
}

bool Telegram::sendText(const String &message) {
   String response;

    const String query =
      "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) +
      "&text=" + urlEncode(message) +
      "&parse_mode=HTML";

   if (!request("sendMessage", query, response)) {
      return false;
   }

   DynamicJsonDocument doc(2048);

   if (deserializeJson(doc, response.substring(response.indexOf("{\"ok\"")))) {
      return false;
   }

   return doc["ok"] | false;
}

bool Telegram::sendSticker(const String &stickerId, int32_t &messageId) {
   messageId = 0;

   String response;

   const String query =
      "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) +
      "&sticker=" + urlEncode(stickerId);

   if (!request("sendSticker", query, response)) {
      return false;
   }

   DynamicJsonDocument doc(2048);

   if (deserializeJson(doc, response.substring(response.indexOf("{\"ok\"")))) {
      return false;
   }

   if (!(doc["ok"] | false)) {
      return false;
   }

   messageId = doc["result"]["message_id"] | 0;

   // Diagnostic-only: confirm the exact API field that feeds lastFallStickerId /
   // lastReturnStickerId in TelegramNotifier.
   Serial.printf("[%lu] TG: sendSticker result.message_id=%ld\n", static_cast<unsigned long>(millis()), static_cast<long>(messageId));

   return messageId > 0;
}

bool Telegram::deleteMessage(int32_t messageId) {
   if (messageId <= 0) {
      // Diagnostic-only: the id handed to the API was not usable.
      Serial.printf("[%lu] TG: deleteMessage id_invalido=%ld\n", static_cast<unsigned long>(millis()), static_cast<long>(messageId));
      return false;
   }

   String response;

   const String query =
      "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) +
      "&message_id=" + String(messageId);

   if (!request("deleteMessage", query, response)) {
      return false;
   }

   DynamicJsonDocument doc(1024);

   if (deserializeJson(doc, response.substring(response.indexOf("{\"ok\"")))) {
      // Diagnostic-only: the reply body was not parseable JSON.
      Serial.printf("[%lu] TG: deleteMessage json_nao_parseavel id=%ld\n", static_cast<unsigned long>(millis()), static_cast<long>(messageId));
      return false;
   }

   const bool apiOk = doc["ok"] | false;

   // Diagnostic-only: the parsed API verdict for this deletion.
   Serial.printf("[%lu] TG: deleteMessage resultado_api ok=%d id=%ld\n", static_cast<unsigned long>(millis()), apiOk ? 1 : 0, static_cast<long>(messageId));

   if (apiOk) {
      return true;
   }

   // Telegram answers this specific error when the target message no longer
   // exists: the deletion is already satisfied, so it counts as a logical success
   // and TelegramNotifier::stepDelOld() clears the stored id and advances phase
   // exactly as after a confirmed deletion. Every other outcome (other API error,
   // other HTTP status, transport failure) keeps the current retry behaviour.
   const int errorCode = doc["error_code"] | 0;
   const String description = doc["description"] | "";

   if (errorCode == 400 && description.indexOf("message to delete not found") >= 0) {
      Serial.printf("[%lu] TG: deleteMessage ja_inexistente id=%ld\n", static_cast<unsigned long>(millis()), static_cast<long>(messageId));
      return true;
   }

   return false;
}

void Telegram::processUpdates(const String &response) {
   const int jsonStart = response.indexOf("{\"ok\"");

   if (jsonStart < 0) {
      return;
   }

   DynamicJsonDocument doc(8192);

   if (deserializeJson(doc, response.substring(jsonStart))) {
      return;
   }

   if (!(doc["ok"] | false)) {
      return;
   }

   JsonArray results = doc["result"].as<JsonArray>();

   for (JsonObject update : results) {
      updateOffset_ = update["update_id"].as<int32_t>() + 1;

      const char *text = update["message"]["text"] | "";

      // chat.id arrives as a JSON number (ours does not fit in 32 bits), so it must be
      // compared numerically: read as a string the number yields the default "" and
      // every message would be dropped by the check below.
      const long long chatId = update["message"]["chat"]["id"] | 0LL;

      if (chatId != strtoll(TELEGRAM_CHAT_ID, nullptr, 10)) {
         continue;
      }

      String command = String(text);
      command.trim();
      command.toLowerCase();

      if (command == "/reboot") {
         rebootRequested_ = true;
      } else if (command == "/ota") {
         otaRequested_ = true;
      }
   }
}

void Telegram::update(uint32_t now) {
   if (WiFi.status() != WL_CONNECTED) {
      return;
   }

   // Polled at most once every TELEGRAM_GETUPDATES_INTERVAL_MS, counted from the END
   // of the previous getUpdates(). millis() is read here instead of using the caller's
   // `now`: that value is sampled at the top of the main loop and may already be
   // seconds old, which would let the next poll start before the interval really
   // elapsed. The unsigned subtraction is wrap-safe.
   if (static_cast<uint32_t>(millis() - lastPoll_) < TELEGRAM_GETUPDATES_INTERVAL_MS) {
      return;
   }

   String response;

   String query;

   if (firstPoll_) {
      // Ignore a stale backlog after a long offline period. The first poll
      // starts at the newest update so an old /reboot cannot unexpectedly
      // restart the device.
      query = "offset=-1&limit=1&timeout=0";
   } else {
      query =
         "offset=" + String(updateOffset_) +
         "&limit=5&timeout=0";
   }

   if (request("getUpdates", query, response, false)) {
      processUpdates(response);
      firstPoll_ = false;
   }

   // Stamped after the call returned, so the interval is measured end -> next start.
   lastPoll_ = millis();
}

bool Telegram::commandRebootRequested() {
   const bool result = rebootRequested_;
   rebootRequested_ = false;
   return result;
}

bool Telegram::commandOtaRequested() {
   const bool result = otaRequested_;
   otaRequested_ = false;
   return result;
}

int32_t Telegram::updateOffset() const {
   return updateOffset_;
}
