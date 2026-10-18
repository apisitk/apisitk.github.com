#pragma once
#include "store.h"

// Bank slip reading. The board only sends the Telegram file_id; Apps Script
// downloads the photo, Typhoon OCR reads it and Typhoon LLM turns the text
// into fields (apps_script/Code.gs, action "slip").
namespace slip {

struct Info {
  int32_t amt = 0;  // satang, always positive; direction is decided here
  uint32_t ts = 0;  // transfer time from the slip, 0 if unreadable
  String from, fromBank, to, toBank, ref, memo;
};

// Shared reader from secrets.h; "" disables it.
void begin(const char *url, const char *secret);
bool enabled(const User &u);
// Tries the user's own sheet script first (so their slips stay in their own
// Google account), then the shared reader.
bool read(const User &u, const String &fileId, Info &out, String &err);

// +1 income (money came to one of the account's names), -1 expense, 0 unsure.
int direction(const Account &a, const Info &s);
bool nameMatch(const String &slipName, const String &mine);

}  // namespace slip
