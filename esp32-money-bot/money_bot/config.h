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

#define GAS_TIMEOUT_MS      20000             // Apps Script can be slow to wake up
#define BACKUP_BATCH        40                // rows per request
#define BACKUP_MAX_ROUNDS   5                 // batches per file per sync, keeps the bot responsive
#define BACKUP_IDLE_MS      (60UL * 1000)     // check the queue this often
#define BACKUP_RETRY_MS     (60UL * 1000)     // first retry after a failure, doubles...
#define BACKUP_RETRY_MAX_MS (30UL * 60 * 1000)  // ...up to this

#define SLIP_TIMEOUT_MS     60000             // download + OCR + extraction in Apps Script
#define SLIP_DRAFT_MS       (60UL * 60 * 1000)  // unconfirmed slip drafts expire after this
#define SLIP_MAX_DRAFTS     10
