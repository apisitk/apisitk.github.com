#pragma once
#include "config.h"
#include <Arduino.h>
#include <vector>

// Minimal Telegram Bot API client: long-poll getUpdates + sendMessage.
namespace tg {

struct Update {
  int64_t updateId = 0;
  int64_t chatId = 0;
  int64_t fromId = 0;
  String fromName;
  String text;
  bool isPrivate = false;
};

void begin(const char *token);
// Blocks up to timeoutSec. Returns false on network/API error.
bool poll(int64_t offset, int timeoutSec, std::vector<Update> &out);
bool send(int64_t chatId, const String &text);

}  // namespace tg
