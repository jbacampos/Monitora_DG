#include "telegram.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "secrets.h"

namespace {
   constexpr char TELEGRAM_HOST[] = "api.telegram.org";
}

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

   if (WiFi.status() != WL_CONNECTED) {
      return false;
   }

   WiFiClientSecure client;
   client.setInsecure();
   client.setTimeout(TELEGRAM_RESPONSE_TIMEOUT_MS);

   if (!client.connect(TELEGRAM_HOST, 443)) {
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

   return response.indexOf("{\"ok\"") >= 0;
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
   return messageId > 0;
}

bool Telegram::deleteMessage(int32_t messageId) {
   if (messageId <= 0) {
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
      return false;
   }

   return doc["ok"] | false;
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
