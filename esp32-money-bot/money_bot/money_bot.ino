// ESP32 Telegram bot for income/expense records.
// See ../README.md for setup.
#include "config.h"
#include "secrets.h"
#include "bot.h"
#include "store.h"
#include "tg.h"
#include <Preferences.h>
#include <WiFi.h>

struct BootUser {
  int64_t id;
  const char *name;
};
static const BootUser BOOT[] = BOOT_USERS;

static Preferences prefs;
static int64_t offset = 0;
static unsigned long wifiLostAt = 0;

static void firstBoot() {
  Serial.println("[boot] first boot, creating users");
  for (const BootUser &b : BOOT) store::addUser(b.id, b.name);
  String err;
  Account *shop = store::create(BOOT[0].id, BOOT_SHARED_KEY, BOOT_SHARED_NAME, false, err);
  if (!shop) {
    Serial.printf("[boot] shared account: %s\n", err.c_str());
    return;
  }
  for (size_t i = 1; i < sizeof BOOT / sizeof BOOT[0]; i++) store::share(*shop, BOOT[i].id, err);
  for (User &u : store::users()) u.cur = shop->id;
  store::save();
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] connecting");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.printf(" %s\n", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "failed");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!store::begin()) {
    Serial.println("[boot] LittleFS failed, restarting");
    delay(5000);
    ESP.restart();
  }
  if (store::empty()) firstBoot();

  prefs.begin("bot", false);
  offset = prefs.getLong64("offset", 0);

  connectWiFi();
  configTzTime(TZ_INFO, NTP_1, NTP_2);
  tg::begin(BOT_TOKEN);
  Serial.println("[boot] ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostAt) wifiLostAt = millis();
    if (millis() - wifiLostAt > WIFI_REBOOT_MS) ESP.restart();
    delay(1000);
    return;
  }
  wifiLostAt = 0;

  std::vector<tg::Update> updates;
  if (!tg::poll(offset, POLL_TIMEOUT_S, updates)) {
    delay(3000);
    return;
  }
  for (const tg::Update &u : updates) {
    // Advance and persist the offset first, so a message that crashes the
    // bot is skipped after reboot instead of crashing it forever.
    offset = u.updateId + 1;
    prefs.putLong64("offset", offset);
    bot::handle(u);
  }
}
