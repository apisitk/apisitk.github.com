#pragma once
#include "config.h"
#include <Arduino.h>

// Google Apps Script web app client (backup now, slip reading later).
namespace gas {

// POSTs JSON to the web app and returns the response body.
// Apps Script answers with a 302 to script.googleusercontent.com; followed.
bool post(const String &url, const String &body, String &resp);

}  // namespace gas
