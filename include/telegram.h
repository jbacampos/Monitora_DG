#pragma once

#include <Arduino.h>

class Telegram {
public:
   bool begin();

   void update(uint32_t now);

   bool sendText(const String &message);
   bool sendSticker(const String &stickerId, int32_t &messageId);
   bool deleteMessage(int32_t messageId);

   bool commandRebootRequested();
   bool commandOtaRequested();

private:
   uint32_t lastPoll_ = 0;
   int32_t updateOffset_ = 0;
   bool firstPoll_ = true;

   bool rebootRequested_ = false;
   bool otaRequested_ = false;

   bool request(
      const String &method,
      const String &query,
      String &response
   );

   void processUpdates(const String &response);

   static String urlEncode(const String &value);
};
