#include "bot.h"
#include "backup.h"
#include "store.h"
#include <WiFi.h>

namespace bot {

static const char *HELP =
    "📒 บอทบันทึกรายรับรายจ่าย\n"
    "\n"
    "บันทึก\n"
    "  +500 ขายของ        รายรับ\n"
    "  -120 ค่าน้ำแข็ง      รายจ่าย (หมวดอัตโนมัติ)\n"
    "  -80 กาแฟ @อาหาร #me  ระบุหมวด/บัญชีเอง\n"
    "  /edit 12 -150 @หมวด  แก้รายการ\n"
    "  /del 12  /undo     ลบรายการ\n"
    "\n"
    "ดูยอด\n"
    "  /today [บัญชี]      วันนี้\n"
    "  /month [บัญชี] [2026-10]  ทั้งเดือน\n"
    "  /sum               ทุกบัญชีเดือนนี้\n"
    "  /last [บัญชี] [10]  รายการล่าสุด\n"
    "\n"
    "หมวด\n"
    "  /cat [บัญชี]        ดูหมวดและคำค้น\n"
    "  /addcat ชื่อ [คำค้น..] เพิ่มหมวด/คำค้น\n"
    "  /delcat ชื่อ\n"
    "\n"
    "บัญชี\n"
    "  /acc  /use shop    ดู/เปลี่ยนบัญชี\n"
    "  /newacc key ชื่อ    สร้างบัญชีแชร์\n"
    "  /newpriv key ชื่อ   สร้างบัญชีส่วนตัว\n"
    "  /share key คน  /unshare key คน\n"
    "  /members key\n"
    "\n"
    "Backup (Google Sheet)\n"
    "  /backup            ดูว่าบัญชีไหนไปชีตไหน\n"
    "  /backup URL SECRET  ชีตของคุณ\n"
    "  /backup shop URL SECRET  ชีตแยกของบัญชี\n"
    "  /sync              ส่งขึ้นชีตเดี๋ยวนี้\n"
    "\n"
    "ผู้ใช้\n"
    "  /adduser chat_id ชื่อ  /deluser ชื่อ\n"
    "  /id  /status";

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

static String catName(const String &c) { return c.length() ? c : "อื่นๆ"; }

static String catLines(const std::vector<CatSum> &cats) {
  String s;
  for (const CatSum &c : cats) s += "  • " + catName(c.cat) + " " + money(c.amt) + "\n";
  return s;
}

static String totalsText(const Totals &t) {
  String s = "รับ  +" + money(t.in) + "\n" + catLines(t.inCats);
  s += "จ่าย -" + money(t.out) + "\n" + catLines(t.outCats);
  return s + "คงเหลือ " + money(t.in - t.out) + "\n(" + String(t.count) + " รายการ)";
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

// What a user typed for a transaction: "-120 ค่าน้ำแข็ง @วัตถุดิบ #shop".
struct Entry {
  bool hasAmt = false;
  int32_t amt = 0;
  String note, key, cat;  // key = account, cat = explicit category
};

// Parses tok[i..]. The amount is optional here; callers decide if it's
// required. Returns an error message, or "" on success.
static String parseEntry(User &u, const std::vector<String> &tok, size_t i, Entry &e) {
  if (i < tok.size() && (tok[i][0] == '+' || tok[i][0] == '-')) {
    bool income = tok[i][0] == '+';
    String num = tok[i].substring(1);
    if (num.length() == 0 && i + 1 < tok.size()) num = tok[++i];  // "+ 500"
    if (!parseAmount(num, e.amt)) return "❓ จำนวนเงินไม่ถูกต้อง ตัวอย่าง: -120 ค่าน้ำแข็ง";
    if (!income) e.amt = -e.amt;
    e.hasAmt = true;
    i++;
  }
  for (; i < tok.size(); i++) {
    const String &t = tok[i];
    // "#shop" picks an account only if there is one; otherwise "#1" etc. stays in the note.
    if (t[0] == '#' && t.length() > 1 && store::resolve(u.id, t.substring(1))) {
      e.key = t.substring(1);
      continue;
    }
    if (t[0] == '@' && t.length() > 1) {
      e.cat = t.substring(1);
      continue;
    }
    if (e.note.length()) e.note += ' ';
    e.note += t;
  }
  return "";
}

// Exact category name for an explicit "@cat", or an error message.
static bool checkCat(Account &a, String &cat, String &err) {
  Category *c = a.cat(cat);
  if (!c) {
    err = "❓ ไม่มีหมวด " + cat + " ใน " + a.name + "\nดู /cat หรือเพิ่มด้วย /addcat " + cat;
    return false;
  }
  cat = c->name;
  return true;
}

static String txLine(int32_t amt, const String &note, const String &cat, uint32_t id) {
  return signedMoney(amt) + (note.length() ? " " + note : "") + " 📂" + catName(cat) + " #" + String(id);
}

static String record(User &u, const std::vector<String> &tok) {
  if (!timeReady()) return "⏳ ยังไม่ได้ซิงค์เวลา รอสักครู่แล้วลองใหม่";
  Entry e;
  String err = parseEntry(u, tok, 0, e);
  if (err.length()) return err;
  if (!e.hasAmt) return "❓ จำนวนเงินไม่ถูกต้อง ตัวอย่าง: -120 ค่าน้ำแข็ง";

  Account *a = pick(u, e.key, err);
  if (!a) return err;
  if (e.cat.length()) {
    if (!checkCat(*a, e.cat, err)) return err;
  } else {
    e.cat = a->autoCat(e.note);
  }
  uint32_t id = store::addTx(*a, e.amt, e.note, e.cat, u.id);
  if (!id) return "⚠️ บันทึกไม่สำเร็จ (พื้นที่เต็ม?) ดู /status";

  backup::kick();
  String line = txLine(e.amt, e.note, e.cat, id);
  notifyOthers(*a, u.id, "บันทึก " + line);
  return "✅ " + line + "\n📁 " + a->name + (e.cat.length() ? "" : "\nใส่หมวดด้วย /edit " + String(id) + " @หมวด");
}

// /edit 12 [-150] [note] [@cat] [#acc]: changes only what is given.
static String cmdEdit(User &u, const std::vector<String> &tok) {
  if (tok.size() < 3) return "ใช้: /edit เลขรายการ [-150] [โน้ตใหม่] [@หมวด]";
  uint32_t id = strtoul(tok[1].c_str() + (tok[1][0] == '#'), nullptr, 10);
  Entry e;
  String err = parseEntry(u, tok, 2, e);
  if (err.length()) return err;
  Account *a = pick(u, e.key, err);
  if (!a) return err;
  if (e.cat.length() && !checkCat(*a, e.cat, err)) return err;

  // Apply only the given fields on top of the current row.
  Tx changed;
  if (!store::findTx(*a, id, changed)) return "ไม่พบรายการ #" + String(id) + " ใน " + a->name;
  if (e.hasAmt) changed.amt = e.amt;
  if (e.note.length()) changed.note = e.note;
  if (e.cat.length()) changed.cat = e.cat;

  Tx before;
  if (!store::editTx(*a, id, changed, before)) return "⚠️ แก้ไม่สำเร็จ";
  backup::kick();
  String line = txLine(changed.amt, changed.note, changed.cat, id);
  notifyOthers(*a, u.id, "แก้ " + line);
  return "✏️ " + line;
}

static String cmdCat(User &u, const std::vector<String> &tok) {
  String err;
  Account *a = pick(u, tok.size() > 1 ? tok[1] : "", err);
  if (!a) return err;
  String s = "📂 หมวดใน " + a->name + "\n";
  for (const Category &c : a->cats) {
    s += "• " + c.name;
    if (c.kw.size()) {
      s += ":";
      for (const String &k : c.kw) s += " " + k;
    }
    s += "\n";
  }
  return s + "\nโน้ตที่มีคำเหล่านี้จะลงหมวดให้อัตโนมัติ\nหรือระบุเองด้วย @หมวด";
}

// /addcat name [keywords...]: creates the category or adds keywords to it.
static String cmdAddCat(User &u, const std::vector<String> &tok) {
  if (tok.size() < 2) return "ใช้: /addcat ชื่อหมวด [คำค้น ...]";
  String err;
  Account *a = pick(u, "", err);
  if (!a) return err;
  String name = tok[1][0] == '@' ? tok[1].substring(1) : tok[1];
  if (name.length() == 0 || name.length() > 30) return "ชื่อหมวดยาวเกินไป";
  Category *c = a->cat(name);
  bool fresh = !c;
  if (fresh) {
    if (a->cats.size() >= 20) return "หมวดเต็มแล้ว (20) ลบด้วย /delcat";
    a->cats.push_back({name, {}});
    c = &a->cats.back();
  }
  for (size_t i = 2; i < tok.size() && c->kw.size() < 15; i++) c->kw.push_back(tok[i]);
  store::save();
  return String(fresh ? "✅ เพิ่มหมวด " : "✅ เพิ่มคำค้นให้ ") + c->name + " ใน " + a->name;
}

static String cmdDelCat(User &u, const std::vector<String> &tok) {
  if (tok.size() < 2) return "ใช้: /delcat ชื่อหมวด";
  String err;
  Account *a = pick(u, "", err);
  if (!a) return err;
  String name = tok[1][0] == '@' ? tok[1].substring(1) : tok[1];
  for (size_t i = 0; i < a->cats.size(); i++)
    if (a->cats[i].name.equalsIgnoreCase(name)) {
      String n = a->cats[i].name;
      a->cats.erase(a->cats.begin() + i);
      store::save();
      return "🗑 ลบหมวด " + n + " (รายการเก่ายังเก็บชื่อหมวดไว้)";
    }
  return "ไม่มีหมวด " + name;
}

static String cmdDelUser(User &u, const std::vector<String> &tok) {
  if (tok.size() < 2) return "ใช้: /deluser ชื่อ";
  User *other = store::userByName(tok[1]);
  if (!other) return "ไม่พบผู้ใช้ " + tok[1];
  if (other->id == u.id) return "ลบตัวเองไม่ได้";
  int64_t id = other->id;
  String name = other->name, err;
  if (!store::removeUser(id, err)) return "⚠️ " + err;
  return "✅ ลบ " + name + " แล้ว\nบัญชีส่วนตัวของเขายังเก็บไว้ (ไม่มีใครเห็น) ถ้าเพิ่มกลับจะได้คืน\n"
         "ถ้าเขาอยู่ใน BOOT_USERS ต้องเอาออกจาก secrets.h ด้วย ไม่งั้นบูตใหม่จะกลับมา";
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
    s += " 📂" + catName(t.cat);
    if (!a->priv) s += " (" + nameOf(t.by) + ")";
    s += "\n";
  }
  return s;
}

static String removeTx(User &u, Account &a, uint32_t id) {
  Tx t;
  if (!store::delTx(a, id, t)) return "ไม่พบรายการ #" + String(id) + " ใน " + a.name;
  backup::kick();
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

// /backup                       where each of my accounts is backed up
// /backup URL SECRET             my own sheet (my private accounts + fallback)
// /backup <key> URL SECRET       a sheet just for this account
// /backup [<key>] off
static String cmdBackup(User &u, const std::vector<String> &tok) {
  Account *acc = nullptr;
  size_t i = 1;
  if (tok.size() > 1 && tok[1].indexOf("://") < 0 && !tok[1].equalsIgnoreCase("off")) {
    acc = store::resolve(u.id, tok[1]);
    if (!acc) return "ไม่พบบัญชี " + tok[1];
    i = 2;
  }
  String &url = acc ? acc->bkUrl : u.bkUrl;
  String &secret = acc ? acc->bkSecret : u.bkSecret;
  String what = acc ? "บัญชี " + acc->name : String("ของคุณ");

  if (tok.size() >= i + 2) {
    String err;
    if (!backup::ping(tok[i], tok[i + 1], err)) return "⚠️ ใช้ไม่ได้: " + err;
    url = tok[i];
    secret = tok[i + 1];
    store::save();
    // Push everything that now lands in this sheet, including rows that
    // were synced somewhere else before; the sheet upserts by id.
    for (Account *a : store::visible(u.id)) {
      backup::Dest d;
      if (backup::dest(*a, d) && d.url == url) store::resync(*a);
    }
    backup::kick();
    return "✅ ตั้งชีต" + what + "แล้ว กำลังส่งข้อมูลขึ้นชีต\n🔐 ลบข้อความที่มี secret ทิ้งได้เลย";
  }
  if (tok.size() == i + 1 && tok[i].equalsIgnoreCase("off")) {
    url = secret = "";
    store::save();
    return "⏹ ปิดชีต" + what + "แล้ว (ข้อมูลยังอยู่ในบอร์ด)";
  }
  if (acc) return "ใช้: /backup " + acc->key + " URL SECRET  หรือ  /backup " + acc->key + " off";

  String s = String("☁️ ชีตของคุณ: ") + (u.bkUrl.length() ? "ตั้งแล้ว" : "ยังไม่ตั้ง") + "\n\n";
  for (Account *a : store::visible(u.id)) {
    backup::Dest d;
    s += "📁 " + a->name + " → " + (backup::dest(*a, d) ? d.label : String("ยังไม่มีชีต (รอในบอร์ด)")) + "\n";
  }
  return s + "\nชีตของคุณ: /backup URL SECRET\nชีตแยกต่อบัญชี: /backup shop URL SECRET\nวิธีทำชีต: apps_script/README.md";
}

static String cmdSync() {
  String report;
  backup::now(report);
  return report;
}

static String cmdStatus() {
  char buf[400];  // Thai text is 3 bytes per character
  snprintf(buf, sizeof buf,
           "⚙️ สถานะ\nพื้นที่: %u / %u KB\nRAM ว่าง: %u KB\nWiFi: %d dBm\nเปิดมา: %lu นาที\nเวลา: %s\n",
           (unsigned)(store::fsUsed() / 1024), (unsigned)(store::fsTotal() / 1024),
           (unsigned)(ESP.getFreeHeap() / 1024), WiFi.RSSI(), millis() / 60000,
           timeReady() ? dateTime(time(nullptr)).c_str() : "ยังไม่ซิงค์");
  return buf + backup::status();
}

void handle(const tg::Update &up) {
  // Private chats only: a group would leak private accounts to everyone in it.
  if (!up.isPrivate) return;

  char idbuf[24];
  snprintf(idbuf, sizeof idbuf, "%lld", (long long)up.fromId);

  User *u = store::user(up.fromId);
  if (!u) {
    tg::send(up.chatId, "🔒 ยังไม่ได้รับอนุญาต\nchat id ของคุณ: " + String(idbuf) +
                            "\nให้สมาชิกพิมพ์ /adduser " + idbuf + " ชื่อ");
    return;
  }

  if (up.text.length() == 0) {  // photo, sticker, voice, ...
    tg::send(up.chatId, "ตอนนี้รับได้แค่ข้อความ (อ่านรูปสลิปจะมาเร็วๆ นี้)\nพิมพ์ /help ดูวิธีใช้");
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
  else if (cmd == "/edit") reply = cmdEdit(*u, tok);
  else if (cmd == "/cat") reply = cmdCat(*u, tok);
  else if (cmd == "/addcat") reply = cmdAddCat(*u, tok);
  else if (cmd == "/delcat") reply = cmdDelCat(*u, tok);
  else if (cmd == "/deluser") reply = cmdDelUser(*u, tok);
  else if (cmd == "/backup") reply = cmdBackup(*u, tok);
  else if (cmd == "/sync") reply = cmdSync();
  else if (cmd == "/status") reply = cmdStatus();
  else reply = "❓ ไม่เข้าใจ พิมพ์ /help";

  tg::send(up.chatId, reply);
}

}  // namespace bot
