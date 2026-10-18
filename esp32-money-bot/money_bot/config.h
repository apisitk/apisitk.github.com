#pragma once

// Must be the same in every translation unit, so it lives here.
#define ARDUINOJSON_USE_LONG_LONG 1

#define TZ_INFO        "ICT-7"   // Thailand, UTC+7, no DST
#define NTP_1          "pool.ntp.org"
#define NTP_2          "time.google.com"

#define POLL_TIMEOUT_S 25        // Telegram long-poll timeout
#define POLL_LIMIT     5         // updates per getUpdates call
#define KEEP_MONTHS    13        // how many months /del and /last search back
#define TX_NOTE_MAX    80        // bytes of note kept per transaction
#define WIFI_REBOOT_MS (5UL * 60 * 1000)  // reboot if WiFi is down this long
