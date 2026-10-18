#pragma once
#include "config.h"
#include <Arduino.h>
#include <vector>

// Minimal Telegram Bot API client: long-poll getUpdates, send/edit
// messages, inline keyboard callbacks.
namespace tg {

struct Update {
  int64_t updateId = 0;
  int64_t chatId = 0;
  int64_t fromId = 0;
  String fromName;
  String text;
  bool isPrivate = false;
  String photoId;  // largest photo, or an image sent as a file (for OCR)
  String keepId;   // largest size within SLIP_KEEP_MAX_BYTES (to store in Drive)
  String caption;  // text under the photo
  // Inline keyboard button press (callback_query):
  String cbId, cbData;
  int32_t cbMsgId = 0;  // message the button belongs to
};

void begin(const char *token);
// Blocks up to timeoutSec. Returns false on network/API error.
bool poll(int64_t offset, int timeoutSec, std::vector<Update> &out);
// `keyboard` is a JSON inline_keyboard array, "" for none.
// Returns the new message id, 0 on failure.
int32_t send(int64_t chatId, const String &text, const String &keyboard = "");
bool edit(int64_t chatId, int32_t msgId, const String &text, const String &keyboard = "");
void answer(const String &cbId, const String &text = "");  // stops the button spinner

}  // namespace tg
