#pragma once

#include <Arduino.h>

class Telegram {
public:
   // restoredUpdateOffset is the persisted update_id + 1 (0 when nothing is known yet).
   // It is restored so an update already processed before a reboot is not executed again.
   bool begin(int32_t restoredUpdateOffset = 0);

   void update(uint32_t now);

   // Drops the persistent Telegram TLS connection (if any). Called when a
   // notification sequence ends and immediately before OTA.
   void closeConnection();

   bool sendText(const String &message);
   bool sendSticker(const String &stickerId, int32_t &messageId);
   bool deleteMessage(int32_t messageId);

   bool commandRebootRequested();
   bool commandOtaRequested();

   // update_id + 1 of the last processed batch. The caller persists it when a received
   // command is about to reboot the device, before doing so.
   int32_t updateOffset() const;

   // Diagnostic counters, read by the periodic DIAG line.
   uint32_t reuseAttempts() const;
   uint32_t reuseSuccess() const;
   uint32_t reuseStalls() const;
   uint32_t newConnections() const;

private:
   uint32_t lastPoll_ = 0;
   int32_t updateOffset_ = 0;
   bool firstPoll_ = true;

   bool rebootRequested_ = false;
   bool otaRequested_ = false;

   bool request(
      const String &method,
      const String &query,
      String &response,
      bool allowReuse = true
   );

   void processUpdates(const String &response);

   static String urlEncode(const String &value);
};
