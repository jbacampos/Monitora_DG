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

   // Diagnostic-only: numeric HTTP status, or 0 when there is no HTTP status
   // line. Never touches the URL/token.
   int diagHttpStatus(const String &response) {
      if (response.length() < 12 || response.indexOf("HTTP/") != 0) {
         return 0;
      }

      const int sp = response.indexOf(' ');

      if (sp < 0 || sp + 4 > static_cast<int>(response.length())) {
         return 0;
      }

      return response.substring(sp + 1, sp + 4).toInt();
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

bool Telegram::begin() {
   lastPoll_ = 0;
   updateOffset_ = 0;
   firstPoll_ = true;
   rebootRequested_ = false;
   otaRequested_ = false;
   return true;
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
   String &response) {

   const uint32_t httpStart = millis();

   Serial.printf("[%lu] TG HTTP: %s inicio\n", static_cast<unsigned long>(httpStart), method.c_str());

   heapLog(method.c_str(), "pre-new");

   if (WiFi.status() != WL_CONNECTED) {
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "NO_WIFI", httpStart);
      return false;
   }

   WiFiClientSecure client;
   client.setInsecure();
   client.setTimeout(TELEGRAM_RESPONSE_TIMEOUT_MS);

   // BearSSL I/O buffers for this per-call Telegram TLS client. Default is
   // setBufferSizes(16384, 512) -> 16709 B (in) + 597 B (out); the Telegram
   // requests/responses are small, so 4096 payload bytes (-> 4421 + 597 B) fit
   // alongside the ThingsBoard TLS client.
   client.setBufferSizes(4096, 512);

   heapLog(method.c_str(), "pre-connect");

   if (!client.connect(TELEGRAM_HOST, 443)) {
      heapLog(method.c_str(), "pos-connect-fail");
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "CONNECT_FAIL", httpStart);
      return false;
   }

   const String path =
      "/bot" + String(TELEGRAM_BOT_TOKEN) +
      "/" + method +
      (query.isEmpty() ? "" : "?" + query);

   client.print(
      String("GET ") + path + " HTTP/1.1\r\n"
      "Host: " + TELEGRAM_HOST + "\r\n"
      "Connection: close\r\n\r\n"
   );

   const uint32_t start = millis();

   while (!client.available() &&
          static_cast<uint32_t>(millis() - start) < TELEGRAM_CONNECT_TIMEOUT_MS) {
      yield();
   }

   if (!client.available()) {
      client.stop();
      heapLog(method.c_str(), "pos-close-nodata");
      httpLog(method.c_str(), false, 0, httpStart);
      httpTransportLog(method.c_str(), "NO_DATA_TIMEOUT", httpStart);
      return false;
   }

   response.reserve(1024);

   while ((client.connected() || client.available()) &&
          static_cast<uint32_t>(millis() - start) < TELEGRAM_RESPONSE_TIMEOUT_MS) {
      while (client.available()) {
         response += static_cast<char>(client.read());
      }
      yield();
   }

   client.stop();

   heapLog(method.c_str(), "pos-close");

   const bool ok = response.indexOf("{\"ok\"") >= 0;
   const int http = diagHttpStatus(response);

   httpLog(method.c_str(), ok, http, httpStart);
   httpResponseLog(method.c_str(), response, http, httpStart);

   heapLog(method.c_str(), "pos-resposta");

   return ok;
}

bool Telegram::sendText(const String &message) {
   String response;

   const String query =
      "chat_id=" + urlEncode(TELEGRAM_CHAT_ID) +
      "&text=" + urlEncode(message);

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

   return apiOk;
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
      const char *chatId = update["message"]["chat"]["id"] | "";

      if (String(chatId) != String(TELEGRAM_CHAT_ID)) {
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

   if (static_cast<uint32_t>(now - lastPoll_) < TELEGRAM_POLL_MS) {
      return;
   }

   lastPoll_ = now;

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

   if (request("getUpdates", query, response)) {
      processUpdates(response);
      firstPoll_ = false;
   }
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
