#pragma once

#include <Arduino.h>

class Telegram {
public:
   // Verdict of deleteMessage(). The caller MUST tell "the message is gone" apart from
   // "Telegram refuses to delete it", because the second one is permanent (message older
   // than 48 h, or a bot without the can_delete_messages right) and retrying can never
   // succeed. Treating it as a transient failure used to block DEL_OLD forever, which also
   // blocked the getUpdates() poll and therefore every command (/reboot, /ota).
   enum class DeleteResult : uint8_t {
      Deleted,    // {"ok":true,"result":true}
      NotFound,   // "message to delete not found": the deletion is already satisfied
      Impossible, // Telegram refuses permanently (age/rights): drop the id and go on
      Failed      // transient (transport, 5xx, 429, unconfirmed reply): retry later
   };

   // restoredUpdateOffset is the persisted update_id + 1 (0 when nothing is known yet).
   // It is restored so an update already processed before a reboot is not executed again.
   bool begin(int32_t restoredUpdateOffset = 0);

   void update(uint32_t now);

   // Drops the persistent Telegram TLS connection (if any). Called when a
   // notification sequence ends and immediately before OTA.
   void closeConnection();

   bool sendText(const String &message);
   // sendMessage() variant that also reports the created message_id, so the caller can
   // replace (delete) the previous visual message. Returns the API ok flag; messageId is
   // set to 0 when the reply carried no id.
   bool sendText(const String &message, int32_t &messageId);
   bool sendSticker(const String &stickerId, int32_t &messageId);

   // Returns Deleted/NotFound when the id no longer refers to a live message,
   // Impossible when Telegram will never accept the deletion and Failed when the
   // deletion may still succeed on a later attempt.
   DeleteResult deleteMessage(int32_t messageId);

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

   // Verdict of the LAST API reply parsed by this class. lastErrorCode() is 0 when the
   // previous call failed before any API reply (a transport problem, transient by
   // definition). lastErrorFatal() is true only for the errors that can never succeed by
   // retrying (401 bad token, 403 no rights/kicked, 400 chat not found or invalid sticker id,
   // 409 webhook conflict): the caller must abandon the operation instead of retrying it
   // forever. lastRetryAfterSeconds() carries Telegram's retry_after on a 429.
   int lastErrorCode() const;
   bool lastErrorFatal() const;
   uint16_t lastRetryAfterSeconds() const;

private:
   uint32_t lastPoll_ = 0;
   int32_t updateOffset_ = 0;
   bool firstPoll_ = true;

   bool rebootRequested_ = false;
   bool otaRequested_ = false;

   int lastErrorCode_ = 0;
   bool lastErrorFatal_ = false;
   uint16_t lastRetryAfterSeconds_ = 0;

   // Records the verdict of an API reply so the caller can tell a permanent failure from a
   // transient one. Called by every method that parses the reply.
   void noteApiResult(int errorCode, const String &description, uint16_t retryAfterSeconds);

   bool request(
      const String &method,
      const String &query,
      String &response,
      bool allowReuse = true
   );

   // Shared sendMessage() implementation. When outId is non-null it receives the
   // message_id from the Telegram reply (0 when the reply carried no id).
   bool sendMessageInternal(const String &message, int32_t *outId);

   void processUpdates(const String &response);

   static String urlEncode(const String &value);
};
