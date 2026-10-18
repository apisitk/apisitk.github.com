// Copy this file to secrets.h and fill in your values.
// secrets.h is in .gitignore -- never commit it.
#pragma once

#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-password"

// From @BotFather
#define BOT_TOKEN "123456789:AAxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Users created on first boot only: {telegram chat id, name}.
// Not sure of your chat id? Flash with any value, then send /id to the bot.
// More users can be added later in chat with /adduser.
#define BOOT_USERS { {111111111LL, "me"}, {222222222LL, "fan"} }

// Shared account created on first boot, with all BOOT_USERS as members.
#define BOOT_SHARED_KEY  "shop"
#define BOOT_SHARED_NAME "ร้าน"
