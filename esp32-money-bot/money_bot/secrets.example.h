// Copy this file to secrets.h and fill in your values.
// secrets.h is in .gitignore -- never commit it.
#pragma once

#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-password"

// From @BotFather
#define BOT_TOKEN "123456789:AAxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Users added at boot if missing: {telegram chat id, name}.
// Not sure of your chat id? Flash with any value, send /id to the bot, fix
// it here and flash again. More users can be added in chat with /adduser.
#define BOOT_USERS { {111111111LL, "me"}, {222222222LL, "fan"} }

// Shared account created at boot if missing, with all BOOT_USERS as members.
#define BOOT_SHARED_KEY  "shop"
#define BOOT_SHARED_NAME "ร้าน"

// Slip reader (step 5): an Apps Script web app with TYPHOON_KEY and BOT_TOKEN
// in its Script Properties (apps_script/README.md). Used for users whose own
// sheet script has no Typhoon key. "" = off.
#define SLIP_URL    ""
#define SLIP_SECRET ""
