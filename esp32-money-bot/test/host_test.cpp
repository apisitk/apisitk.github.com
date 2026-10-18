// Runs the real bot/store code on a PC against a fake Telegram.
// Build & run: ./run.sh
#include "../money_bot/backup.h"
#include "../money_bot/bot.h"
#include "../money_bot/gas.h"
#include <ArduinoJson.h>
#include "../money_bot/store.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <cassert>
#include <iostream>
#include <map>

SerialShim Serial;
EspShim ESP;
WiFiShim WiFi;
LittleFSShim LittleFS;
std::string FS_ROOT;

// Fake Telegram: remember what was sent to whom.
static std::map<int64_t, std::vector<std::string>> inbox;
namespace tg {
void begin(const char *) {}
bool poll(int64_t, int, std::vector<Update> &) { return true; }
bool send(int64_t chat, const String &text) {
  inbox[chat].push_back(text.s);
  return true;
}
}  // namespace tg

// Fake Apps Script web apps, keyed by URL. Mirrors Code.gs: checks the
// secret, upserts rows by id per sheet, deletes rows marked del.
struct FakeSheet {
  std::string secret;
  std::map<std::string, std::map<uint32_t, std::string>> tabs;  // tab -> id -> "amt|cat|note|by"
  int calls = 0;
};
static std::map<std::string, FakeSheet> sheets;
static bool googleDown = false;

namespace gas {
bool post(const String &url, const String &body, String &resp) {
  if (googleDown || !sheets.count(url.s)) return false;
  FakeSheet &sh = sheets[url.s];
  sh.calls++;
  JsonDocument req;
  deserializeJson(req, body.s);
  if (req["secret"] != sh.secret.c_str()) {
    resp = "{\"ok\":false,\"error\":\"wrong secret\"}";
    return true;
  }
  for (JsonObject r : req["rows"].as<JsonArray>()) {
    auto &tab = sh.tabs[r["sheet"].as<std::string>()];
    uint32_t id = r["id"];
    if (r["del"] | false) tab.erase(id);
    else
      tab[id] = std::to_string(r["amt"].as<int>()) + "|" + r["cat"].as<std::string>() + "|" +
                r["note"].as<std::string>() + "|" + r["by"].as<std::string>();
  }
  resp = "{\"ok\":true}";
  return true;
}
}  // namespace gas

static size_t rowsIn(const std::string &url) {
  size_t n = 0;
  for (auto &t : sheets[url].tabs) n += t.second.size();
  return n;
}
static bool hasRow(const std::string &url, const std::string &tab, uint32_t id) {
  auto &tabs = sheets[url].tabs;
  return tabs.count(tab) && tabs[tab].count(id);
}

static const int64_t ME = 111, FAN = 222, STRANGER = 999;
static int failures = 0;

// Sends `text` as `from`, returns the bot's reply to that chat.
static std::string say(int64_t from, const std::string &text, bool isPrivate = true) {
  inbox.clear();
  tg::Update u;
  u.chatId = u.fromId = from;
  u.text = text.c_str();
  u.isPrivate = isPrivate;
  bot::handle(u);
  std::string r = inbox[from].empty() ? "" : inbox[from].back();
  std::cout << "\n[" << from << "] " << text << "\n" << r << "\n";
  return r;
}

static void expect(const std::string &reply, const std::string &part, const char *what) {
  if (reply.find(part) == std::string::npos) {
    std::cout << "  ❌ FAIL: " << what << " (expected \"" << part << "\")\n";
    failures++;
  }
}
static void expectNot(const std::string &reply, const std::string &part, const char *what) {
  if (reply.find(part) != std::string::npos) {
    std::cout << "  ❌ FAIL: " << what << " (unexpected \"" << part << "\")\n";
    failures++;
  }
}

int main() {
  setenv("TZ", "ICT-7", 1);
  tzset();
  FS_ROOT = std::filesystem::temp_directory_path() / "money_bot_test_fs";
  std::filesystem::remove_all(FS_ROOT);
  store::begin();

  // Same as syncBootUsers() with BOOT_USERS = me, fan and a shared "shop".
  store::addUser(ME, "me");
  store::addUser(FAN, "fan");
  String err;
  Account *shop = store::create(ME, "shop", "ร้าน", false, err);
  store::share(*shop, FAN, err);
  store::user(ME)->cur = store::user(FAN)->cur = shop->id;
  store::save();

  // Access control
  std::string r = say(STRANGER, "+100 hack");
  expect(r, "ยังไม่ได้รับอนุญาต", "stranger is rejected");
  expect(r, "999", "stranger gets their chat id");
  inbox.clear();
  tg::Update g;
  g.chatId = -5;
  g.fromId = ME;
  g.text = "/sum";
  g.isPrivate = false;
  bot::handle(g);
  if (!inbox.empty()) { std::cout << "  ❌ FAIL: bot answered in a group\n"; failures++; }

  // Recording + auto category + notification
  r = say(ME, "-120 ค่าน้ำแข็ง");
  expect(r, "-120.00", "expense amount");
  expect(r, "📂วัตถุดิบ", "auto category from keyword");
  if (inbox[FAN].empty()) { std::cout << "  ❌ FAIL: fan not notified\n"; failures++; }
  r = say(FAN, "+1,500.5 ขายของ");
  expect(r, "+1,500.50", "income with comma/decimal");
  expect(r, "📂ขาย", "income category");
  r = say(ME, "-35 อะไรไม่รู้");
  expect(r, "📂อื่นๆ", "uncategorized");
  expect(r, "/edit", "hint to set category");
  r = say(ME, "-50 ถุงมือ @บรรจุภัณฑ์");
  expect(r, "📂บรรจุภัณฑ์", "explicit category");
  r = say(ME, "-50 x @ไม่มี");
  expect(r, "ไม่มีหมวด", "unknown explicit category rejected");
  r = say(ME, "-20 โต๊ะ #1");
  expect(r, "#1", "#1 stays in note");
  expectNot(r, "ไม่พบบัญชี", "#1 is not an account");

  // Bad amounts
  for (const char *bad : {"-", "+0", "-12.345", "-abc", "+ "}) expect(say(ME, bad), "จำนวนเงินไม่ถูกต้อง", bad);

  // Thai note longer than 80 bytes must still be valid UTF-8
  std::string longNote = "-10 ";
  for (int i = 0; i < 40; i++) longNote += "ก";
  say(ME, longNote);
  r = say(ME, "/last 1");
  for (size_t i = 0; i < r.size();) {
    unsigned char c = r[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || i + n > r.size()) { std::cout << "  ❌ FAIL: broken UTF-8 in /last\n"; failures++; break; }
    i += n;
  }

  // Private account
  r = say(ME, "-65 กาแฟ #me");
  expect(r, "📂อาหาร", "personal category");
  expect(r, "ส่วนตัว me", "went to my private account");
  if (!inbox[FAN].empty()) { std::cout << "  ❌ FAIL: fan notified about private tx\n"; failures++; }
  r = say(FAN, "/acc");
  expectNot(r, "ส่วนตัว me", "fan cannot see my private account");
  r = say(FAN, "/sum");
  expectNot(r, "ส่วนตัว me", "fan /sum excludes my private");
  r = say(FAN, "/share me fan");
  expect(r, "แชร์ไม่ได้", "fan's me is their own private account");
  r = say(ME, "/share me fan");
  expect(r, "แชร์ไม่ได้", "private accounts cannot be shared");
  uint32_t privId = 0;
  { std::vector<Tx> t = store::recent(*store::resolve(ME, "me"), 1); privId = t[0].id; }
  r = say(FAN, "/del " + std::to_string(privId));
  expect(r, "ไม่พบรายการ", "fan cannot delete my private tx");
  r = say(FAN, "/edit " + std::to_string(privId) + " -1");
  expect(r, "ไม่พบรายการ", "fan cannot edit my private tx");

  // Summary with categories
  r = say(ME, "/today");
  expect(r, "รับ  +1,500.50", "today income");
  expect(r, "• วัตถุดิบ 120.00", "category line");
  expect(r, "คงเหลือ", "balance");

  // Edit
  Tx last;
  store::lastBy(*shop, ME, last);
  r = say(ME, "/edit " + std::to_string(last.id) + " -25 @วัตถุดิบ");
  expect(r, "-25.00", "edit amount");
  expect(r, "📂วัตถุดิบ", "edit category");
  r = say(ME, "/edit 99999 -1");
  expect(r, "ไม่พบรายการ", "edit unknown id");

  // Undo / del
  r = say(ME, "/undo");
  expect(r, "ลบ", "undo");
  r = say(FAN, "/del 1");
  expect(r, "ลบ", "fan deletes shared tx");
  r = say(FAN, "/del 1");
  expect(r, "ไม่พบรายการ", "already deleted");

  // Categories
  r = say(ME, "/addcat ค่าส่ง flash kerry ไปรษณีย์");
  expect(r, "เพิ่มหมวด ค่าส่ง", "add category");
  r = say(ME, "-40 ส่ง kerry");
  expect(r, "📂ค่าส่ง", "new keyword matches");
  r = say(ME, "/delcat ค่าส่ง");
  expect(r, "ลบหมวด", "delete category");
  r = say(ME, "/cat");
  expectNot(r, "ค่าส่ง", "category gone");

  // Accounts
  r = say(FAN, "/newpriv saving เงินเก็บ");
  expect(r, "สร้าง", "fan creates private account");
  r = say(ME, "/use saving");
  expect(r, "ไม่พบบัญชี", "I cannot use fan's private account");
  r = say(ME, "/newacc saving");
  expect(r, "สร้าง", "same key for my shared account is fine");

  // Users
  r = say(FAN, "/adduser 333 mom");
  expect(r, "เพิ่ม mom", "add user");
  r = say(ME, "/deluser me");
  expect(r, "ลบตัวเองไม่ได้", "cannot delete self");
  r = say(ME, "/deluser mom");
  expect(r, "ลบ mom", "delete user");
  expect(say(333, "/acc"), "ยังไม่ได้รับอนุญาต", "deleted user locked out");

  // Unsupported message
  expect(say(ME, ""), "รับได้แค่ข้อความ", "photo/sticker reply");

  // Survives a reboot (reload db from disk)
  store::begin();
  shop = store::account("shop");  // old pointers die with the reload, like on a real reboot
  r = say(FAN, "/acc");
  expect(r, "เงินเก็บ", "accounts reloaded");
  r = say(ME, "/month");
  expect(r, "• บรรจุภัณฑ์ 50.00", "transactions reloaded");

  // ---------------- Backup (step 4) ----------------
  const std::string URL_ME = "https://script.google.com/macros/s/ME/exec";
  const std::string URL_SHOP = "https://script.google.com/macros/s/SHOP/exec";
  const std::string URL_FAN = "https://script.google.com/macros/s/FAN/exec";
  sheets[URL_ME].secret = "s-me";
  sheets[URL_SHOP].secret = "s-shop";
  sheets[URL_FAN].secret = "s-fan";
  auto check = [](bool ok, const char *what) {
    if (!ok) { std::cout << "  ❌ FAIL: " << what << "\n"; failures++; }
  };
  String rep;

  say(FAN, "-30 ข้าวมันไก่ #me");  // fan's private, before anyone has a sheet
  r = say(ME, "/backup");
  expect(r, "ยังไม่มีชีต", "no sheet yet");
  backup::now(rep);
  check(rowsIn(URL_ME) + rowsIn(URL_SHOP) + rowsIn(URL_FAN) == 0, "nothing sent without a sheet");
  check(!store::pending().empty(), "rows wait on the board");

  expect(say(ME, "/backup http://evil.com/x s"), "ต้องขึ้นต้นด้วย", "only script.google.com");
  expect(say(ME, "/backup " + URL_ME + " nope"), "wrong secret", "bad secret rejected");
  expect(say(ME, "/backup https://script.google.com/macros/s/NOPE/exec s"), "ติดต่อไม่ได้", "unreachable url");

  r = say(ME, "/backup " + URL_ME + " s-me");
  expect(r, "ตั้งชีตของคุณแล้ว", "personal sheet set");
  backup::now(rep);
  Account *myMe = store::resolve(ME, "me"), *fanMe = store::resolve(FAN, "me");
  check(sheets[URL_ME].tabs.count("ส่วนตัว me (me)") == 1, "my private tab in my sheet");
  check(sheets[URL_ME].tabs.count("ร้าน (shop)") == 1, "shop falls back to first member with a sheet");
  size_t liveShop = store::recent(*shop, 1000).size();
  check(sheets[URL_ME].tabs["ร้าน (shop)"].size() == liveShop, "all live shop rows sent, deleted ones not");
  for (auto &t : sheets[URL_ME].tabs)
    for (auto &row : t.second) check(row.second.find("ข้าวมันไก่") == std::string::npos, "fan's private NOT in my sheet");

  r = say(FAN, "/backup");
  expect(r, "ร้าน → ชีตของ me", "fan sees where shop goes");
  expect(r, "ส่วนตัว fan → ยังไม่มีชีต", "fan's private waits for fan's own sheet");

  r = say(ME, "/backup shop " + URL_SHOP + " s-shop");
  expect(r, "ตั้งชีตบัญชี ร้าน", "per-account sheet set");
  backup::now(rep);
  check(sheets[URL_SHOP].tabs["ร้าน (shop)"].size() == liveShop, "shop resent to its own sheet");
  check(sheets[URL_SHOP].tabs.count("ส่วนตัว me (me)") == 0, "my private not in the shop sheet");
  expect(say(FAN, "/backup"), "ร้าน → ชีตของบัญชีนี้", "shop now has its own sheet");

  r = say(FAN, "/backup " + URL_FAN + " s-fan");
  backup::now(rep);
  check(sheets[URL_FAN].tabs.count("ส่วนตัว fan (me)") == 1, "fan's private reaches fan's sheet");
  check(sheets[URL_FAN].tabs.count("ร้าน (shop)") == 0, "shop does not go to fan's sheet");
  check(store::pending().empty(), "queue empty after sync");

  // Offline, then back: nothing lost, edits and deletes follow
  googleDown = true;
  say(ME, "-99 น้ำแข็ง");
  Tx t99;
  store::lastBy(*shop, ME, t99);
  check(!backup::now(rep), "sync fails while offline");
  expect(rep.s, "ส่งไม่สำเร็จ", "offline report");
  expect(say(ME, "/status"), "sync ล่าสุดพลาด", "/status shows the failure");
  store::begin();  // reboot while offline
  shop = store::account("shop");
  check(!store::pending().empty(), "queue survives reboot");
  googleDown = false;
  check(backup::now(rep), "sync ok after network is back");
  check(hasRow(URL_SHOP, "ร้าน (shop)", t99.id), "offline row arrived");
  say(ME, "/edit " + std::to_string(t99.id) + " -111 @ค่าน้ำไฟ");
  backup::now(rep);
  check(sheets[URL_SHOP].tabs["ร้าน (shop)"][t99.id].rfind("-11100|ค่าน้ำไฟ", 0) == 0, "edit reaches the sheet");
  say(FAN, "/del " + std::to_string(t99.id));
  backup::now(rep);
  check(!hasRow(URL_SHOP, "ร้าน (shop)", t99.id), "delete reaches the sheet");
  int callsBefore = sheets[URL_SHOP].calls;
  backup::now(rep);
  check(sheets[URL_SHOP].calls == callsBefore, "no requests when nothing changed");

  // More than one batch
  for (int i = 0; i < 95; i++) say(ME, "+1 ขาย" + std::to_string(i));
  backup::now(rep);
  check(store::pending().empty(), "95 rows in one /sync (3 batches)");
  check(sheets[URL_SHOP].tabs["ร้าน (shop)"].size() == store::recent(*shop, 1000).size(), "sheet matches board");

  r = say(ME, "/backup shop off");
  expect(r, "ปิดชีตบัญชี ร้าน", "per-account off");
  expect(say(ME, "/backup"), "ร้าน → ชีตของ me", "falls back to my sheet");
  (void)myMe;
  (void)fanMe;

  std::cout << "\n" << (failures ? "❌ " : "✅ ") << failures << " failure(s)\n";
  return failures != 0;
}
