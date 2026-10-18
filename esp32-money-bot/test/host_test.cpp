// Runs the real bot/store code on a PC against a fake Telegram.
// Build & run: ./run.sh
#include "../money_bot/bot.h"
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
  r = say(FAN, "/acc");
  expect(r, "เงินเก็บ", "accounts reloaded");
  r = say(ME, "/month");
  expect(r, "• บรรจุภัณฑ์ 50.00", "transactions reloaded");

  std::cout << "\n" << (failures ? "❌ " : "✅ ") << failures << " failure(s)\n";
  return failures != 0;
}
