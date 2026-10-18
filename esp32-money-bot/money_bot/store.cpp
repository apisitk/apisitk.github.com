#include "store.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <algorithm>

static const char *DB_PATH = "/db.json";

static const char *PENDING_PATH = "/pending.txt";

static std::deque<User> gUsers;
static std::vector<String> gPending;
static std::deque<Account> gAccounts;
static Preferences prefs;

bool Account::has(int64_t uid) const {
  for (int64_t m : members)
    if (m == uid) return true;
  return false;
}

Category *Account::cat(const String &name) {
  for (Category &c : cats)
    if (c.name.equalsIgnoreCase(name)) return &c;
  return nullptr;
}

String Account::autoCat(const String &note) const {
  String n = note;
  n.toLowerCase();
  for (const Category &c : cats) {
    String cn = c.name;
    cn.toLowerCase();
    if (n.indexOf(cn) >= 0) return c.name;
    for (String k : c.kw) {
      k.toLowerCase();
      if (k.length() && n.indexOf(k) >= 0) return c.name;
    }
  }
  return "";
}

struct DefaultCat {
  const char *name;
  const char *kw;  // space separated
};

static const DefaultCat SHOP_CATS[] = {
    {"ขาย", "ขาย ลูกค้า โอนเข้า"},
    {"วัตถุดิบ", "วัตถุดิบ น้ำแข็ง ผัก หมู ไก่ ไข่ แป้ง น้ำตาล แม็คโคร makro"},
    {"บรรจุภัณฑ์", "ถุง แก้ว กล่อง หลอด ฝา"},
    {"ค่าเช่า", "เช่า"},
    {"ค่าน้ำไฟ", "ไฟ ค่าน้ำ ประปา เน็ต แก๊ส"},
    {"ค่าแรง", "ค่าแรง ลูกจ้าง พนักงาน"},
};

static const DefaultCat PERSONAL_CATS[] = {
    {"อาหาร", "ข้าว กาแฟ ชา ขนม อาหาร grab food lineman"},
    {"เดินทาง", "น้ำมัน รถ bts mrt แท็กซี่ taxi ทางด่วน วิน"},
    {"ของใช้", "ของใช้ 7-11 เซเว่น lotus โลตัส บิ๊กซี shopee lazada"},
    {"บิล", "ค่าโทร มือถือ เน็ต ไฟ ค่าน้ำ ประกัน บัตร"},
    {"รายได้", "เงินเดือน โบนัส รายได้"},
};

template <size_t N>
static std::vector<Category> defaults(const DefaultCat (&list)[N]) {
  std::vector<Category> out;
  for (const DefaultCat &d : list) {
    Category c{d.name, {}};
    String kw = d.kw;
    int start = 0;
    while (start < (int)kw.length()) {
      int sp = kw.indexOf(' ', start);
      if (sp < 0) sp = kw.length();
      if (sp > start) c.kw.push_back(kw.substring(start, sp));
      start = sp + 1;
    }
    out.push_back(c);
  }
  return out;
}

static String i64(int64_t v) {
  char buf[24];
  snprintf(buf, sizeof buf, "%lld", (long long)v);
  return buf;
}

// "202610" for the month containing t (local time).
static String monthKey(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  char buf[24];
  snprintf(buf, sizeof buf, "%04d%02d", lt.tm_year + 1900, lt.tm_mon + 1);
  return buf;
}

// Month key `back` months before the month containing t.
static String monthBack(time_t t, int back) {
  struct tm lt;
  localtime_r(&t, &lt);
  int m = lt.tm_year * 12 + lt.tm_mon - back;
  char buf[24];
  snprintf(buf, sizeof buf, "%04d%02d", m / 12 + 1900, m % 12 + 1);
  return buf;
}

static String txPath(const Account &a, const String &ym) {
  return "/tx/" + a.id + "/" + ym + ".jsonl";
}

// Cut to at most maxBytes without splitting a UTF-8 character (Thai is 3
// bytes each); Telegram rejects messages containing broken UTF-8.
static String utf8Cut(const String &s, size_t maxBytes) {
  if (s.length() <= maxBytes) return s;
  size_t n = maxBytes;
  while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
  return s.substring(0, n);
}

// Replace dst with src. LittleFS rename overwrites atomically; fall back to
// remove + rename if this FS layer refuses.
static bool replaceFile(const String &src, const String &dst) {
  if (LittleFS.rename(src, dst)) return true;
  LittleFS.remove(dst);
  return LittleFS.rename(src, dst);
}

static bool validKey(const String &k) {
  if (k.length() < 1 || k.length() > 16) return false;
  if (k[0] < 'a' || k[0] > 'z') return false;
  for (char c : k)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}

// ---------- transactions on disk ----------

static String toLine(const Tx &t) {
  JsonDocument d;
  d["i"] = t.id;
  d["t"] = t.ts;
  d["a"] = t.amt;
  d["n"] = t.note;
  if (t.cat.length()) d["c"] = t.cat;
  if (t.ref.length()) d["r"] = t.ref;
  d["b"] = t.by;
  if (t.del) d["d"] = 1;
  d["s"] = t.synced ? 1 : 0;
  String s;
  serializeJson(d, s);
  return s;
}

static bool fromLine(const String &line, Tx &t) {
  JsonDocument d;
  if (deserializeJson(d, line)) return false;
  t.id = d["i"];
  t.ts = d["t"];
  t.amt = d["a"];
  t.note = d["n"] | "";
  t.cat = d["c"] | "";
  t.ref = d["r"] | "";
  t.by = d["b"].as<int64_t>();
  t.del = d["d"] | 0;
  t.synced = d["s"] | 0;
  return t.id != 0;
}

// Calls fn for every parsed line of a month file. fn returns false to stop.
template <typename F>
static void eachTx(const String &path, F fn) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    Tx t;
    if (fromLine(line, t) && !fn(t)) break;
  }
  f.close();
}

// ---------- backup queue ----------

static void savePending() {
  File f = LittleFS.open("/pending.tmp", "w");
  if (!f) return;
  for (const String &k : gPending) f.println(k);
  f.close();
  replaceFile("/pending.tmp", PENDING_PATH);
}

static void loadPending() {
  gPending.clear();
  File f = LittleFS.open(PENDING_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String k = f.readStringUntil('\n');
    if (k.length()) gPending.push_back(k);
  }
  f.close();
}

static void markPending(const Account &a, const String &ym) {
  String k = a.id + "/" + ym;
  for (const String &p : gPending)
    if (p == k) return;
  gPending.push_back(k);
  savePending();
}

// ---------- db.json ----------

static void load() {
  gUsers.clear();
  gAccounts.clear();
  // A power cut between writing db.tmp and renaming it leaves only db.tmp.
  if (!LittleFS.exists(DB_PATH) && LittleFS.exists("/db.tmp")) LittleFS.rename("/db.tmp", DB_PATH);
  File f = LittleFS.open(DB_PATH, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) {
    Serial.printf("[store] db.json parse error: %s\n", err.c_str());
    return;
  }
  for (JsonObject u : d["users"].as<JsonArray>())
    gUsers.push_back({u["id"].as<int64_t>(), u["n"] | "", u["c"] | "", u["bu"] | "", u["bs"] | ""});
  for (JsonObject a : d["accs"].as<JsonArray>()) {
    Account acc{a["id"] | "", a["k"] | "", a["n"] | "", a["p"] | false, {}, {}, a["bu"] | "", a["bs"] | "", {}};
    for (JsonVariant n : a["nm"].as<JsonArray>()) acc.myNames.push_back(n | "");
    for (JsonVariant m : a["m"].as<JsonArray>()) acc.members.push_back(m.as<int64_t>());
    if (a["c"].isNull()) acc.cats = acc.priv ? defaults(PERSONAL_CATS) : defaults(SHOP_CATS);  // db from before categories
    for (JsonObject c : a["c"].as<JsonArray>()) {
      Category cat{c["n"] | "", {}};
      for (JsonVariant k : c["k"].as<JsonArray>()) cat.kw.push_back(k | "");
      acc.cats.push_back(cat);
    }
    gAccounts.push_back(acc);
  }
}

namespace store {

bool begin() {
  if (!LittleFS.begin(true)) return false;  // formats on first use
  prefs.begin("store", false);
  load();
  loadPending();
  return true;
}

bool save() {
  JsonDocument d;
  JsonArray us = d["users"].to<JsonArray>();
  for (const User &u : gUsers) {
    JsonObject o = us.add<JsonObject>();
    o["id"] = u.id;
    o["n"] = u.name;
    o["c"] = u.cur;
    if (u.bkUrl.length()) {
      o["bu"] = u.bkUrl;
      o["bs"] = u.bkSecret;
    }
  }
  JsonArray as = d["accs"].to<JsonArray>();
  for (const Account &a : gAccounts) {
    JsonObject o = as.add<JsonObject>();
    o["id"] = a.id;
    o["k"] = a.key;
    o["n"] = a.name;
    o["p"] = a.priv;
    if (a.bkUrl.length()) {
      o["bu"] = a.bkUrl;
      o["bs"] = a.bkSecret;
    }
    JsonArray m = o["m"].to<JsonArray>();
    for (int64_t id : a.members) m.add(id);
    if (a.myNames.size()) {
      JsonArray nm = o["nm"].to<JsonArray>();
      for (const String &n : a.myNames) nm.add(n);
    }
    JsonArray cs = o["c"].to<JsonArray>();
    for (const Category &c : a.cats) {
      JsonObject co = cs.add<JsonObject>();
      co["n"] = c.name;
      JsonArray k = co["k"].to<JsonArray>();
      for (const String &w : c.kw) k.add(w);
    }
  }
  // Write to a temp file first so a power cut never leaves a half-written db.
  File f = LittleFS.open("/db.tmp", "w");
  if (!f) return false;
  bool ok = serializeJson(d, f) > 0;
  f.close();
  if (!ok) return false;
  return replaceFile("/db.tmp", DB_PATH);
}

std::deque<User> &users() { return gUsers; }

User *user(int64_t id) {
  for (User &u : gUsers)
    if (u.id == id) return &u;
  return nullptr;
}

User *userByName(const String &name) {
  for (User &u : gUsers)
    if (u.name.equalsIgnoreCase(name)) return &u;
  return nullptr;
}

User &addUser(int64_t id, const String &name) {
  gUsers.push_back({id, name, "", "", ""});
  String err;
  Account *me = resolve(id, "me");  // left over from an earlier /deluser
  if (!me) me = create(id, "me", "ส่วนตัว " + name, true, err);
  User &u = gUsers.back();
  if (me) u.cur = me->id;
  save();
  return u;
}

bool removeUser(int64_t id, String &err) {
  for (const Account &a : gAccounts)
    if (!a.priv && a.has(id) && a.members.size() == 1) {
      err = "เป็นสมาชิกคนเดียวของ " + a.name + " — แชร์ให้คนอื่นก่อน";
      return false;
    }
  for (Account &a : gAccounts)
    if (!a.priv)
      for (size_t i = 0; i < a.members.size(); i++)
        if (a.members[i] == id) a.members.erase(a.members.begin() + i--);
  for (size_t i = 0; i < gUsers.size(); i++)
    if (gUsers[i].id == id) gUsers.erase(gUsers.begin() + i--);
  return save();
}

Account *account(const String &id) {
  for (Account &a : gAccounts)
    if (a.id == id) return &a;
  return nullptr;
}

Account *resolve(int64_t uid, const String &key) {
  String k = key;
  k.toLowerCase();
  for (Account &a : gAccounts)
    if (a.key == k && a.has(uid)) return &a;
  return nullptr;
}

std::vector<Account *> visible(int64_t uid) {
  std::vector<Account *> out;
  for (Account &a : gAccounts)
    if (a.has(uid)) out.push_back(&a);
  return out;
}

Account *create(int64_t owner, const String &key, const String &name, bool priv, String &err) {
  String k = key;
  k.toLowerCase();
  if (!validKey(k)) {
    err = "ชื่อบัญชีใช้ได้แค่ a-z 0-9 _ ขึ้นต้นด้วยตัวอักษร ยาวไม่เกิน 16";
    return nullptr;
  }
  if (resolve(owner, k)) {
    err = "คุณมีบัญชีชื่อ " + k + " อยู่แล้ว";
    return nullptr;
  }
  // Private ids get the owner id appended ('-' is not allowed in keys),
  // so two people can both have a private "me" without clashing.
  String id = priv ? k + "-" + i64(owner) : k;
  if (account(id)) {
    err = "ชื่อ " + k + " ถูกใช้แล้ว ลองชื่ออื่น";
    return nullptr;
  }
  gAccounts.push_back({id, k, name.length() ? name : k, priv, {owner},
                       priv ? defaults(PERSONAL_CATS) : defaults(SHOP_CATS), "", "", {}});
  save();
  return &gAccounts.back();
}

bool share(Account &a, int64_t uid, String &err) {
  if (a.priv) {
    err = "บัญชีส่วนตัวแชร์ไม่ได้";
    return false;
  }
  if (a.has(uid)) {
    err = "เป็นสมาชิกอยู่แล้ว";
    return false;
  }
  if (resolve(uid, a.key)) {
    err = "อีกฝ่ายมีบัญชีชื่อ " + a.key + " อยู่แล้ว";
    return false;
  }
  a.members.push_back(uid);
  return save();
}

bool unshare(Account &a, int64_t uid, String &err) {
  if (!a.has(uid)) {
    err = "ไม่ได้เป็นสมาชิก";
    return false;
  }
  if (a.members.size() == 1) {
    err = "ต้องเหลือสมาชิกอย่างน้อย 1 คน";
    return false;
  }
  for (size_t i = 0; i < a.members.size(); i++)
    if (a.members[i] == uid) a.members.erase(a.members.begin() + i);
  User *u = user(uid);
  if (u && u->cur == a.id) {
    Account *me = resolve(uid, "me");
    u->cur = me ? me->id : "";
  }
  return save();
}

uint32_t addTx(const Account &a, int32_t amt, const String &note, const String &cat, int64_t by, uint32_t ts,
               const String &ref) {
  Tx t;
  t.id = prefs.getUInt("txid", 0) + 1;
  t.ts = ts ? ts : time(nullptr);
  t.ref = ref;
  t.amt = amt;
  t.note = utf8Cut(note, TX_NOTE_MAX);
  t.cat = cat;
  t.by = by;
  File f = LittleFS.open(txPath(a, monthKey(t.ts)), "a", true);
  if (!f) return 0;
  bool ok = f.println(toLine(t)) > 0;
  f.close();
  if (!ok) return 0;
  prefs.putUInt("txid", t.id);
  markPending(a, monthKey(t.ts));
  return t.id;
}

// Finds live tx `id` in the account's recent months and rewrites its month
// file with fn applied to it. The row is marked unsynced so the change
// reaches the backup later. `before` gets the row as it was.
template <typename F>
static bool rewriteTx(const Account &a, uint32_t id, Tx &before, F fn) {
  time_t now = time(nullptr);
  for (int back = 0; back < KEEP_MONTHS; back++) {
    String path = txPath(a, monthBack(now, back));
    bool found = false;
    eachTx(path, [&](Tx &t) {
      if (t.id == id && !t.del) {
        before = t;
        found = true;
      }
      return !found;
    });
    if (!found) continue;

    String tmp = path + ".tmp";
    File out = LittleFS.open(tmp, "w");
    if (!out) return false;
    eachTx(path, [&](Tx &t) {
      if (t.id == id) {
        fn(t);
        t.synced = false;
      }
      out.println(toLine(t));
      return true;
    });
    out.close();
    if (!replaceFile(tmp, path)) return false;
    markPending(a, monthBack(now, back));
    return true;
  }
  return false;
}

bool findRef(const Account &a, const String &ref, Tx &out) {
  if (!ref.length()) return false;
  time_t now = time(nullptr);
  for (int back = 0; back < KEEP_MONTHS; back++) {
    bool found = false;
    eachTx(txPath(a, monthBack(now, back)), [&](Tx &t) {
      if (t.ref == ref && !t.del) {
        out = t;
        found = true;
      }
      return !found;
    });
    if (found) return true;
  }
  return false;
}

bool findTx(const Account &a, uint32_t id, Tx &out) {
  time_t now = time(nullptr);
  for (int back = 0; back < KEEP_MONTHS; back++) {
    bool found = false;
    eachTx(txPath(a, monthBack(now, back)), [&](Tx &t) {
      if (t.id == id && !t.del) {
        out = t;
        found = true;
      }
      return !found;
    });
    if (found) return true;
  }
  return false;
}

bool delTx(const Account &a, uint32_t id, Tx &removed) {
  return rewriteTx(a, id, removed, [](Tx &t) { t.del = true; });
}

bool editTx(const Account &a, uint32_t id, const Tx &changed, Tx &before) {
  return rewriteTx(a, id, before, [&](Tx &t) {
    t.amt = changed.amt;
    t.note = utf8Cut(changed.note, TX_NOTE_MAX);
    t.cat = changed.cat;
  });
}

bool lastBy(const Account &a, int64_t by, Tx &out) {
  // Highest id = added last. Not "last line of the newest month": slips are
  // filed under the month on the slip, which may be long ago.
  time_t now = time(nullptr);
  bool found = false;
  for (int back = 0; back < KEEP_MONTHS; back++)
    eachTx(txPath(a, monthBack(now, back)), [&](Tx &t) {
      if (t.by == by && !t.del && (!found || t.id > out.id)) {
        out = t;
        found = true;
      }
      return true;
    });
  return found;
}

std::vector<Tx> recent(const Account &a, size_t n) {
  std::vector<Tx> out;
  time_t now = time(nullptr);
  for (int back = 0; back < KEEP_MONTHS && out.size() < n; back++) {
    std::vector<Tx> month;
    eachTx(txPath(a, monthBack(now, back)), [&](Tx &t) {
      if (!t.del) month.push_back(t);
      return true;
    });
    for (auto it = month.rbegin(); it != month.rend() && out.size() < n; ++it) out.push_back(*it);
  }
  return out;
}

Totals totals(const Account &a, time_t from, time_t to) {
  Totals tot;
  String last = monthKey(to - 1);
  for (int back = 0;; back++) {
    String ym = monthBack(from, -back);  // walk forward from `from`
    eachTx(txPath(a, ym), [&](Tx &t) {
      if (!t.del && (time_t)t.ts >= from && (time_t)t.ts < to) {
        std::vector<CatSum> &cats = t.amt > 0 ? tot.inCats : tot.outCats;
        int64_t amt = t.amt > 0 ? t.amt : -(int64_t)t.amt;
        (t.amt > 0 ? tot.in : tot.out) += amt;
        tot.count++;
        bool merged = false;
        for (CatSum &c : cats)
          if (c.cat == t.cat) {
            c.amt += amt;
            merged = true;
          }
        if (!merged) cats.push_back({t.cat, amt});
      }
      return true;
    });
    if (ym == last || back > 120) break;
  }
  auto bigFirst = [](const CatSum &x, const CatSum &y) { return x.amt > y.amt; };
  std::sort(tot.inCats.begin(), tot.inCats.end(), bigFirst);
  std::sort(tot.outCats.begin(), tot.outCats.end(), bigFirst);
  return tot;
}

const std::vector<String> &pending() { return gPending; }

Account *unsynced(const String &key, size_t max, std::vector<Tx> &out) {
  int slash = key.indexOf('/');
  Account *a = account(key.substring(0, slash));
  if (!a) return nullptr;
  eachTx(txPath(*a, key.substring(slash + 1)), [&](Tx &t) {
    if (!t.synced) out.push_back(t);
    return out.size() < max;
  });
  return a;
}

bool markSynced(const String &key, const std::vector<uint32_t> &ids) {
  int slash = key.indexOf('/');
  Account *a = account(key.substring(0, slash));
  if (!a) return false;
  String path = txPath(*a, key.substring(slash + 1)), tmp = path + ".tmp";
  File out = LittleFS.open(tmp, "w");
  if (!out) return false;
  bool left = false;
  eachTx(path, [&](Tx &t) {
    for (uint32_t id : ids)
      if (t.id == id) t.synced = true;
    left |= !t.synced;
    out.println(toLine(t));
    return true;
  });
  out.close();
  if (!replaceFile(tmp, path)) return false;
  if (!left) dropPending(key);
  return true;
}

void dropPending(const String &key) {
  for (size_t i = 0; i < gPending.size(); i++)
    if (gPending[i] == key) {
      gPending.erase(gPending.begin() + i);
      savePending();
      return;
    }
}

void resync(const Account &a) {
  File dir = LittleFS.open("/tx/" + a.id);
  if (!dir) return;
  std::vector<String> months;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String p = f.path();
    String name = p.substring(p.lastIndexOf('/') + 1);
    f.close();
    if (name.endsWith(".jsonl")) months.push_back(name.substring(0, name.length() - 6));
  }
  dir.close();
  for (const String &ym : months) {
    String path = txPath(a, ym), tmp = path + ".tmp";
    File out = LittleFS.open(tmp, "w");
    if (!out) continue;
    eachTx(path, [&](Tx &t) {
      t.synced = false;
      out.println(toLine(t));
      return true;
    });
    out.close();
    if (replaceFile(tmp, path)) markPending(a, ym);
  }
}

size_t fsUsed() { return LittleFS.usedBytes(); }
size_t fsTotal() { return LittleFS.totalBytes(); }

}  // namespace store
