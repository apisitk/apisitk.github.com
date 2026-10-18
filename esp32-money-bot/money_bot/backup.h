#pragma once
#include "store.h"

// Pushes unsynced transactions to Google Sheets through each user's own
// Apps Script web app (see ../apps_script/Code.gs).
//
// Where an account goes:
//   its own sheet, if one was set with /backup <key> URL SECRET
//   private -> else its owner's sheet, nobody else's
//   shared  -> else the first member (in join order) who has set up a sheet
namespace backup {

struct Dest {
  String url, secret, label;
};
bool dest(const Account &a, Dest &d);  // false = no sheet yet, rows wait on the board
void kick();                        // sync soon (after a change)
void loop();                        // call from loop(); syncs when due
// Syncs now. Returns false if anything failed; `report` says what happened.
bool now(String &report);
// Checks a web app URL + secret before saving them.
bool ping(const String &url, const String &secret, String &err);
String status();  // one line for /status

}  // namespace backup
