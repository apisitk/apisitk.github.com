#include "store.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

static const char *DB_PATH = "/db.json";

static std::deque<User> gUsers;
static std::deque<Account> gAccounts;
static Preferences prefs;

bool Account::has(int64_t uid) const {
  for (int64_t m : members)
    if (m == uid) return true;
  return false;
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

// ---------- db.json ----------

static void load() {
  gUsers.clear();
  gAccounts.clear();
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
    gUsers.push_back({u["id"].as<int64_t>(), u["n"] | "", u["c"] | ""});
  for (JsonObject a : d["accs"].as<JsonArray>()) {
    Account acc{a["id"] | "", a["k"] | "", a["n"] | "", a["p"] | false, {}};
    for (JsonVariant m : a["m"].as<JsonArray>()) acc.members.push_back(m.as<int64_t>());
    gAccounts.push_back(acc);
  }
}

namespace store {

bool begin() {
  if (!LittleFS.begin(true)) return false;  // formats on first use
  prefs.begin("store", false);
  load();
  return true;
}

bool empty() { return gUsers.empty(); }

bool save() {
  JsonDocument d;
  JsonArray us = d["users"].to<JsonArray>();
  for (const User &u : gUsers) {
    JsonObject o = us.add<JsonObject>();
    o["id"] = u.id;
    o["n"] = u.name;
    o["c"] = u.cur;
  }
  JsonArray as = d["accs"].to<JsonArray>();
  for (const Account &a : gAccounts) {
    JsonObject o = as.add<JsonObject>();
    o["id"] = a.id;
    o["k"] = a.key;
    o["n"] = a.name;
    o["p"] = a.priv;
    JsonArray m = o["m"].to<JsonArray>();
    for (int64_t id : a.members) m.add(id);
  }
  // Write to a temp file first so a power cut never leaves a half-written db.
  File f = LittleFS.open("/db.tmp", "w");
  if (!f) return false;
  bool ok = serializeJson(d, f) > 0;
  f.close();
  if (!ok) return false;
  LittleFS.remove(DB_PATH);
  return LittleFS.rename("/db.tmp", DB_PATH);
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
  gUsers.push_back({id, name, ""});
  String err;
  Account *me = create(id, "me", "ส่วนตัว " + name, true, err);
  User &u = gUsers.back();
  if (me) u.cur = me->id;
  save();
  return u;
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
  gAccounts.push_back({id, k, name.length() ? name : k, priv, {owner}});
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

uint32_t addTx(const Account &a, int32_t amt, const String &note, int64_t by) {
  Tx t;
  t.id = prefs.getUInt("txid", 0) + 1;
  t.ts = time(nullptr);
  t.amt = amt;
  t.note = note.substring(0, TX_NOTE_MAX);
  t.by = by;
  File f = LittleFS.open(txPath(a, monthKey(t.ts)), "a", true);
  if (!f) return 0;
  bool ok = f.println(toLine(t)) > 0;
  f.close();
  if (!ok) return 0;
  prefs.putUInt("txid", t.id);
  return t.id;
}

bool delTx(const Account &a, uint32_t id, Tx &removed) {
  time_t now = time(nullptr);
  for (int back = 0; back < KEEP_MONTHS; back++) {
    String path = txPath(a, monthBack(now, back));
    bool found = false;
    eachTx(path, [&](Tx &t) {
      if (t.id == id && !t.del) {
        removed = t;
        found = true;
      }
      return !found;
    });
    if (!found) continue;

    // Rewrite the month file with the row marked deleted (and unsynced,
    // so the deletion reaches the backup later).
    String tmp = path + ".tmp";
    File out = LittleFS.open(tmp, "w");
    if (!out) return false;
    eachTx(path, [&](Tx &t) {
      if (t.id == id) {
        t.del = true;
        t.synced = false;
      }
      out.println(toLine(t));
      return true;
    });
    out.close();
    LittleFS.remove(path);
    return LittleFS.rename(tmp, path);
  }
  return false;
}

bool lastBy(const Account &a, int64_t by, Tx &out) {
  time_t now = time(nullptr);
  for (int back = 0; back < 2; back++) {
    bool found = false;
    eachTx(txPath(a, monthBack(now, back)), [&](Tx &t) {
      if (t.by == by && !t.del) {
        out = t;
        found = true;
      }
      return true;
    });
    if (found) return true;
  }
  return false;
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
        if (t.amt > 0) tot.in += t.amt;
        else tot.out -= t.amt;
        tot.count++;
      }
      return true;
    });
    if (ym == last || back > 120) break;
  }
  return tot;
}

size_t fsUsed() { return LittleFS.usedBytes(); }
size_t fsTotal() { return LittleFS.totalBytes(); }

}  // namespace store
