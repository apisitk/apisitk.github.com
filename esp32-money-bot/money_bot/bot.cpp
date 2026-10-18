#include "bot.h"
#include "store.h"
#include <WiFi.h>

namespace bot {

static const char *HELP =
    "📒 บอทบันทึกรายรับรายจ่าย\n"
    "\n"
    "บันทึก\n"
    "  +500 ขายของ      รายรับ\n"
    "  -120 ค่าน้ำแข็ง    รายจ่าย\n"
    "  -80 กาแฟ #me     ระบุบัญชีในข้อความ\n"
    "\n"
    "ดูยอด\n"
    "  /today [บัญชี]    วันนี้\n"
    "  /month [บัญชี] [2026-10]  ทั้งเดือน\n"
    "  /sum             ทุกบัญชีเดือนนี้\n"
    "  /last [บัญชี] [10] รายการล่าสุด\n"
    "  /del 12  /undo   ลบรายการ\n"
    "\n"
    "บัญชี\n"
    "  /acc             บัญชีที่เห็นได้\n"
    "  /use shop        เปลี่ยนบัญชีที่ใช้\n"
    "  /newacc key ชื่อ  สร้างบัญชีแชร์\n"
    "  /newpriv key ชื่อ สร้างบัญชีส่วนตัว\n"
    "  /share key คน  /unshare key คน\n"
    "  /members key\n"
    "\n"
    "อื่นๆ\n"
    "  /adduser chat_id ชื่อ  /id  /status";

// ---------- helpers ----------

static std::vector<String> split(const String &s) {
  std::vector<String> out;
  int i = 0, n = s.length();
  while (i < n) {
    while (i < n && isspace((unsigned char)s[i])) i++;
    int j = i;
    while (j < n && !isspace((unsigned char)s[j])) j++;
    if (j > i) out.push_back(s.substring(i, j));
    i = j;
  }
  return out;
}

static String joinFrom(const std::vector<String> &v, size_t from) {
  String s;
  for (size_t i = from; i < v.size(); i++) {
    if (s.length()) s += ' ';
    s += v[i];
  }
  return s;
}

// "1,234.5" -> 123450 satang. Rejects zero, junk, more than 2 decimals.
static bool parseAmount(const String &in, int32_t &satang) {
  int64_t whole = 0, frac = 0;
  int fracDigits = -1, digits = 0;
  for (char c : in) {
    if (c == ',') continue;
    if (c == '.' && fracDigits < 0) {
      fracDigits = 0;
      continue;
    }
    if (c < '0' || c > '9') return false;
    if (fracDigits >= 0) {
      if (++fracDigits > 2) return false;
      frac = frac * 10 + (c - '0');
    } else {
      whole = whole * 10 + (c - '0');
      if (++digits > 8) return false;  // < 100M baht
    }
  }
  if (fracDigits == 1) frac *= 10;
  int64_t v = whole * 100 + frac;
  if (v <= 0) return false;
  satang = (int32_t)v;
  return true;
}

static String money(int64_t satang) {
  bool neg = satang < 0;
  uint64_t v = neg ? -satang : satang;
  String whole = String((unsigned long)(v / 100));
  for (int i = whole.length() - 3; i > 0; i -= 3) whole = whole.substring(0, i) + "," + whole.substring(i);
  char frac[4];
  snprintf(frac, sizeof frac, ".%02u", (unsigned)(v % 100));
  return (neg ? "-" : "") + whole + frac;
}

static String signedMoney(int32_t amt) { return (amt > 0 ? "+" : "") + money(amt); }

static String dateTime(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[16];
  strftime(buf, sizeof buf, "%d/%m %H:%M", &lt);
  return buf;
}

static String nameOf(int64_t uid) {
  User *u = store::user(uid);
  return u ? u->name : "?";
}

static String label(const Account &a) { return a.name + " (" + a.key + (a.priv ? ", ส่วนตัว" : "") + ")"; }

static bool timeReady() { return time(nullptr) > 1700000000; }

static time_t startOfDay(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
  return mktime(&lt);
}

static time_t startOfMonth(int year, int month) {  // month 1..12, may overflow
  struct tm lt = {};
  lt.tm_year = year - 1900;
  lt.tm_mon = month - 1;
  lt.tm_mday = 1;
  lt.tm_isdst = -1;
  return mktime(&lt);
}

static String totalsText(const Totals &t) {
  return "รับ  +" + money(t.in) + "\nจ่าย -" + money(t.out) + "\nคงเหลือ " + money(t.in - t.out) + "\n(" +
         String(t.count) + " รายการ)";
}

// Tell the other members of a shared account what happened.
static void notifyOthers(const Account &a, int64_t actor, const String &msg) {
  if (a.priv) return;
  for (int64_t m : a.members)
    if (m != actor) tg::send(m, "🔔 " + nameOf(actor) + " " + msg + " [" + a.name + "]");
}

// Account named by `key`, or the user's current one when key is empty.
static Account *pick(User &u, const String &key, String &reply) {
  Account *a = key.length() ? store::resolve(u.id, key) : store::account(u.cur);
  if (!a || !a->has(u.id)) {
    reply = key.length() ? "ไม่พบบัญชี " + key + " (ดู /acc)" : "ยังไม่ได้เลือกบัญชี ใช้ /use ชื่อบัญชี";
    return nullptr;
  }
  return a;
}

// ---------- handlers ----------

static String record(User &u, const std::vector<String> &tok) {
  if (!timeReady()) return "⏳ ยังไม่ได้ซิงค์เวลา รอสักครู่แล้วลองใหม่";

  // "+500 note" or "+ 500 note"
  String first = tok[0];
  bool income = first[0] == '+';
  String num = first.substring(1);
  size_t rest = 1;
  if (num.length() == 0 && tok.size() > 1) {
    num = tok[1];
    rest = 2;
  }
  int32_t amt;
  if (!parseAmount(num, amt)) return "❓ จำนวนเงินไม่ถูกต้อง ตัวอย่าง: -120 ค่าน้ำแข็ง";
  if (!income) amt = -amt;

  String key, note;
  for (size_t i = rest; i < tok.size(); i++) {
    if (tok[i][0] == '#' && tok[i].length() > 1) {
      key = tok[i].substring(1);
      continue;
    }
    if (note.length()) note += ' ';
    note += tok[i];
  }

  String err;
  Account *a = pick(u, key, err);
  if (!a) return err;
  uint32_t id = store::addTx(*a, amt, note, u.id);
  if (!id) return "⚠️ บันทึกไม่สำเร็จ (พื้นที่เต็ม?) ดู /status";

  String line = signedMoney(amt) + (note.length() ? " " + note : "") + " #" + String(id);
  notifyOthers(*a, u.id, "บันทึก " + line);
  return "✅ " + line + "\n📁 " + a->name;
}

static String cmdAcc(User &u) {
  String s = "บัญชีของคุณ:\n";
  for (Account *a : store::visible(u.id)) s += (a->id == u.cur ? "👉 " : "   ") + label(*a) + "\n";
  return s + "\nเปลี่ยนด้วย /use ชื่อ";
}

static String cmdUse(User &u, const std::vector<String> &tok) {
  if (tok.size() < 2) return cmdAcc(u);
  Account *a = store::resolve(u.id, tok[1]);
  if (!a) return "ไม่พบบัญชี " + tok[1];
  u.cur = a->id;
  store::save();
  return "👉 ตอนนี้ใช้บัญชี " + label(*a);
}

static String cmdNew(User &u, const std::vector<String> &tok, bool priv) {
  if (tok.size() < 2) return priv ? "ใช้: /newpriv key ชื่อ" : "ใช้: /newacc key ชื่อ";
  String err;
  Account *a = store::create(u.id, tok[1], joinFrom(tok, 2), priv, err);
  if (!a) return "⚠️ " + err;
  return "✅ สร้าง " + label(*a) + (priv ? "\nมีแค่คุณที่เห็น" : "\nแชร์ให้คนอื่นด้วย /share " + a->key + " ชื่อคน");
}

static String cmdShare(User &u, const std::vector<String> &tok, bool add) {
  if (tok.size() < 3) return add ? "ใช้: /share key ชื่อคน" : "ใช้: /unshare key ชื่อคน";
  Account *a = store::resolve(u.id, tok[1]);
  if (!a) return "ไม่พบบัญชี " + tok[1];
  User *other = store::userByName(tok[2]);
  if (!other) return "ไม่พบผู้ใช้ " + tok[2];
  String err;
  if (!(add ? store::share(*a, other->id, err) : store::unshare(*a, other->id, err))) return "⚠️ " + err;
  if (add) tg::send(other->id, "📁 " + u.name + " แชร์บัญชี " + label(*a) + " ให้คุณ\nใช้ /use " + a->key);
  return "✅ " + String(add ? "แชร์ให้ " : "เอาออก ") + other->name + " แล้ว";
}

static String cmdMembers(User &u, const std::vector<String> &tok) {
  String err;
  Account *a = pick(u, tok.size() > 1 ? tok[1] : "", err);
  if (!a) return err;
  String s = "สมาชิก " + label(*a) + ":\n";
  for (int64_t m : a->members) s += "• " + nameOf(m) + "\n";
  return s;
}

static String cmdAddUser(const std::vector<String> &tok) {
  if (tok.size() < 3) return "ใช้: /adduser chat_id ชื่อ\n(ให้อีกฝ่ายส่ง /id มาหาบอทก่อน)";
  int64_t id = atoll(tok[1].c_str());
  if (id == 0) return "chat_id ไม่ถูกต้อง";
  if (store::user(id)) return "มีผู้ใช้นี้แล้ว";
  if (store::userByName(tok[2])) return "ชื่อนี้มีแล้ว ใช้ชื่ออื่น";
  User &nu = store::addUser(id, tok[2]);
  tg::send(id, "👋 ยินดีต้อนรับ " + nu.name + "\nคุณมีบัญชีส่วนตัว 'me' แล้ว\n\n" + HELP);
  return "✅ เพิ่ม " + nu.name + " แล้ว แชร์บัญชีให้ด้วย /share key " + nu.name;
}

static String cmdToday(User &u, const std::vector<String> &tok) {
  String err;
  Account *a = pick(u, tok.size() > 1 ? tok[1] : "", err);
  if (!a) return err;
  time_t now = time(nullptr), from = startOfDay(now);
  return "📅 วันนี้ — " + a->name + "\n" + totalsText(store::totals(*a, from, now + 1));
}

static String cmdMonth(User &u, const std::vector<String> &tok) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  int y = lt.tm_year + 1900, m = lt.tm_mon + 1;
  String key;
  for (size_t i = 1; i < tok.size(); i++) {
    int yy, mm;
    if (sscanf(tok[i].c_str(), "%d-%d", &yy, &mm) == 2 && mm >= 1 && mm <= 12) {
      y = yy;
      m = mm;
    } else {
      key = tok[i];
    }
  }
  String err;
  Account *a = pick(u, key, err);
  if (!a) return err;
  char ym[24];
  snprintf(ym, sizeof ym, "%02d/%04d", m, y);
  return "🗓 " + String(ym) + " — " + a->name + "\n" +
         totalsText(store::totals(*a, startOfMonth(y, m), startOfMonth(y, m + 1)));
}

static String cmdSum(User &u) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  time_t from = startOfMonth(lt.tm_year + 1900, lt.tm_mon + 1);
  String s = "🗓 เดือนนี้ทุกบัญชี\n";
  for (Account *a : store::visible(u.id)) {
    Totals t = store::totals(*a, from, now + 1);
    s += "\n📁 " + a->name + "\n" + totalsText(t) + "\n";
  }
  return s;
}

static String cmdLast(User &u, const std::vector<String> &tok) {
  int n = 10;
  String key, err;
  for (size_t i = 1; i < tok.size(); i++) {
    if (isdigit((unsigned char)tok[i][0])) n = constrain(atoi(tok[i].c_str()), 1, 30);
    else key = tok[i];
  }
  Account *a = pick(u, key, err);
  if (!a) return err;
  std::vector<Tx> txs = store::recent(*a, n);
  if (txs.empty()) return "ยังไม่มีรายการใน " + a->name;
  String s = "🧾 ล่าสุด — " + a->name + "\n";
  for (const Tx &t : txs) {
    s += "#" + String(t.id) + " " + dateTime(t.ts) + " " + signedMoney(t.amt);
    if (t.note.length()) s += " " + t.note;
    if (!a->priv) s += " (" + nameOf(t.by) + ")";
    s += "\n";
  }
  return s;
}

static String removeTx(User &u, Account &a, uint32_t id) {
  Tx t;
  if (!store::delTx(a, id, t)) return "ไม่พบรายการ #" + String(id) + " ใน " + a.name;
  String line = signedMoney(t.amt) + (t.note.length() ? " " + t.note : "") + " #" + String(id);
  notifyOthers(a, u.id, "ลบ " + line);
  return "🗑 ลบ " + line;
}

static String cmdDel(User &u, const std::vector<String> &tok) {
  if (tok.size() < 2) return "ใช้: /del เลขรายการ (ดูจาก /last)";
  String err;
  Account *a = pick(u, tok.size() > 2 ? tok[2] : "", err);
  if (!a) return err;
  return removeTx(u, *a, strtoul(tok[1].c_str() + (tok[1][0] == '#'), nullptr, 10));
}

static String cmdUndo(User &u) {
  String err;
  Account *a = pick(u, "", err);
  if (!a) return err;
  Tx t;
  if (!store::lastBy(*a, u.id, t)) return "ไม่มีรายการของคุณให้ยกเลิก";
  return removeTx(u, *a, t.id);
}

static String cmdStatus() {
  char buf[200];
  snprintf(buf, sizeof buf,
           "⚙️ สถานะ\nพื้นที่: %u / %u KB\nRAM ว่าง: %u KB\nWiFi: %d dBm\nเปิดมา: %lu นาที\nเวลา: %s",
           (unsigned)(store::fsUsed() / 1024), (unsigned)(store::fsTotal() / 1024),
           (unsigned)(ESP.getFreeHeap() / 1024), WiFi.RSSI(), millis() / 60000,
           timeReady() ? dateTime(time(nullptr)).c_str() : "ยังไม่ซิงค์");
  return buf;
}

void handle(const tg::Update &up) {
  // Private chats only: a group would leak private accounts to everyone in it.
  if (!up.isPrivate || up.text.length() == 0) return;

  char idbuf[24];
  snprintf(idbuf, sizeof idbuf, "%lld", (long long)up.fromId);

  User *u = store::user(up.fromId);
  if (!u) {
    tg::send(up.chatId, "🔒 ยังไม่ได้รับอนุญาต\nchat id ของคุณ: " + String(idbuf) +
                            "\nให้สมาชิกพิมพ์ /adduser " + idbuf + " ชื่อ");
    return;
  }

  std::vector<String> tok = split(up.text);
  if (tok.empty()) return;
  String cmd = tok[0];
  int at = cmd.indexOf('@');  // "/today@MyBot" in some clients
  if (at > 0) cmd = cmd.substring(0, at);
  cmd.toLowerCase();

  String reply;
  if (cmd[0] == '+' || cmd[0] == '-') reply = record(*u, tok);
  else if (cmd == "/start" || cmd == "/help") reply = HELP;
  else if (cmd == "/id") reply = String("chat id ของคุณ: ") + idbuf;
  else if (cmd == "/acc") reply = cmdAcc(*u);
  else if (cmd == "/use") reply = cmdUse(*u, tok);
  else if (cmd == "/newacc") reply = cmdNew(*u, tok, false);
  else if (cmd == "/newpriv") reply = cmdNew(*u, tok, true);
  else if (cmd == "/share") reply = cmdShare(*u, tok, true);
  else if (cmd == "/unshare") reply = cmdShare(*u, tok, false);
  else if (cmd == "/members") reply = cmdMembers(*u, tok);
  else if (cmd == "/adduser") reply = cmdAddUser(tok);
  else if (cmd == "/today") reply = cmdToday(*u, tok);
  else if (cmd == "/month") reply = cmdMonth(*u, tok);
  else if (cmd == "/sum") reply = cmdSum(*u);
  else if (cmd == "/last") reply = cmdLast(*u, tok);
  else if (cmd == "/del") reply = cmdDel(*u, tok);
  else if (cmd == "/undo") reply = cmdUndo(*u);
  else if (cmd == "/status") reply = cmdStatus();
  else reply = "❓ ไม่เข้าใจ พิมพ์ /help";

  tg::send(up.chatId, reply);
}

}  // namespace bot
