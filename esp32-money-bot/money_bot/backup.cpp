#include "backup.h"
#include "gas.h"
#include <ArduinoJson.h>
#include <algorithm>

namespace backup {

static unsigned long nextAt = 0;  // millis() of the next attempt
static unsigned long backoff = BACKUP_RETRY_MS;
static time_t lastOk = 0;
static String lastErr;

bool dest(const Account &a, Dest &d) {
  if (a.bkUrl.length()) {
    d = {a.bkUrl, a.bkSecret, "ชีตของบัญชีนี้"};
    return true;
  }
  for (int64_t m : a.members) {
    User *u = store::user(m);
    if (u && u->bkUrl.length()) {
      d = {u->bkUrl, u->bkSecret, "ชีตของ " + u->name};
      return true;
    }
    if (a.priv) break;  // private: owner only
  }
  return false;
}

void kick() { nextAt = millis() + 3000; }

static String sheetName(const Account &a) { return a.name == a.key ? a.key : a.name + " (" + a.key + ")"; }

// Drive folder for slip photos: "ร้าน" for a shared account, "ส่วนตัว" for
// the private "me" (other private accounts: "ส่วนตัว <name>"), then the
// script adds <yyyy>/<MM>/<dd>.
static String photoFolder(const Account &a) {
  if (!a.priv) return a.name;
  return a.key == "me" ? String("ส่วนตัว") : "ส่วนตัว " + a.name;
}

// The web app's {"ok":true} check; anything else is an error message.
// `retry` gets the ids the sheet wants again (slip photos it had no time for).
static bool okResponse(const String &resp, String &err, std::vector<uint32_t> *retry = nullptr) {
  JsonDocument d;
  if (deserializeJson(d, resp)) {
    err = "ตอบกลับไม่ใช่ JSON (URL ถูกไหม? deploy เป็น Anyone หรือยัง?)";
    return false;
  }
  if (!(d["ok"] | false)) {
    err = d["error"] | "unknown error";
    return false;
  }
  if (retry)
    for (JsonVariant id : d["retry"].as<JsonArray>()) retry->push_back(id.as<uint32_t>());
  return true;
}

// Sends one batch of one pending file. Returns false on failure; `sent` is
// the number of rows the sheet accepted. Sheets in `failed` already failed
// this round and are skipped, so one broken sheet doesn't hold up the others.
// `more` = the sheet asked for some rows again (photo uploads it had no time
// for); those are sent on the next tick so the bot stays responsive.
static bool sendBatch(const String &key, size_t &sent, bool &more, String &err, std::vector<String> &failed) {
  sent = 0;
  more = false;
  std::vector<Tx> rows;
  Account *a = store::unsynced(key, BACKUP_BATCH, rows);
  if (!a) {
    store::dropPending(key);  // account no longer exists
    return true;
  }
  if (rows.empty()) {
    store::markSynced(key, {});  // nothing left: drop from the queue
    return true;
  }
  Dest to;
  if (!dest(*a, to)) return true;  // nobody has a sheet yet: keep waiting on the board
  for (const String &f : failed)
    if (f == to.url) return true;

  JsonDocument d;
  d["secret"] = to.secret;
  d["action"] = "rows";
  JsonArray arr = d["rows"].to<JsonArray>();
  String sheet = sheetName(*a), folder = photoFolder(*a);
  for (const Tx &t : rows) {
    JsonObject o = arr.add<JsonObject>();
    o["sheet"] = sheet;
    o["id"] = t.id;
    o["ts"] = t.ts;
    o["amt"] = t.amt;  // satang
    o["cat"] = t.cat;
    o["note"] = t.note;
    User *by = store::user(t.by);
    o["by"] = by ? by->name : String("?");
    if (t.ref.length()) o["ref"] = t.ref;
    if (t.photo.length()) {
      o["photo"] = t.photo;
      o["folder"] = folder;
    }
    if (t.del) o["del"] = true;
  }
  String body, resp;
  serializeJson(d, body);
  bool sentOk = gas::post(to.url, body, resp);
  if (!sentOk) err = "ส่งไม่สำเร็จ (เน็ต/URL)";
  std::vector<uint32_t> retry;
  if (!sentOk || !okResponse(resp, err, &retry)) {
    err = to.label + ": " + err;
    failed.push_back(to.url);
    return false;
  }

  std::vector<uint32_t> ids;
  for (const Tx &t : rows)
    if (std::find(retry.begin(), retry.end(), t.id) == retry.end()) ids.push_back(t.id);
  if (!store::markSynced(key, ids)) {
    err = "บันทึกสถานะ sync ลงบอร์ดไม่สำเร็จ (พื้นที่เต็ม?)";
    failed.push_back(to.url);  // rows are in the sheet; resending them later is harmless
    return false;
  }
  sent = ids.size();
  more = !retry.empty();
  return true;
}

bool now(String &report) {
  size_t total = 0, waiting = 0;
  String err;
  std::vector<String> failed;  // sheets that failed this round
  // Copy: sendBatch may drop entries from the queue while we walk it.
  std::vector<String> keys = store::pending();
  for (const String &key : keys) {
    for (int round = 0; round < BACKUP_MAX_ROUNDS; round++) {
      size_t sent;
      bool more;
      String e;
      if (!sendBatch(key, sent, more, e, failed)) {
        if (err.length()) err += "\n";
        err += e;
        break;
      }
      total += sent;
      if (more || sent < BACKUP_BATCH) break;  // file done, waiting for a sheet, or photos next tick
    }
  }
  bool ok = failed.empty();
  for (const String &key : store::pending()) {
    Account *a = store::account(key.substring(0, key.indexOf('/')));
    Dest to;
    if (a && !dest(*a, to)) waiting++;
  }

  if (ok) {
    lastOk = time(nullptr);
    lastErr = "";
  } else {
    lastErr = err;
  }
  report = "☁️ sync แล้ว " + String((unsigned)total) + " รายการ";
  if (!ok) report += "\n⚠️ " + err;
  if (waiting) report += "\n⏸ " + String((unsigned)waiting) + " ไฟล์รอคนตั้ง /backup";
  return ok;
}

void loop() {
  if ((long)(millis() - nextAt) < 0) return;
  bool work = false;
  for (const String &key : store::pending()) {
    Account *a = store::account(key.substring(0, key.indexOf('/')));
    Dest to;
    if (!a || dest(*a, to)) work = true;
  }
  if (!work) {
    nextAt = millis() + BACKUP_IDLE_MS;
    return;
  }
  String report;
  if (now(report)) {
    backoff = BACKUP_RETRY_MS;
    nextAt = millis() + BACKUP_IDLE_MS;
  } else {
    Serial.printf("[backup] %s, retry in %lus\n", report.c_str(), backoff / 1000);
    nextAt = millis() + backoff;
    backoff = min(backoff * 2, (unsigned long)BACKUP_RETRY_MAX_MS);
  }
}

bool ping(const String &url, const String &secret, String &err) {
  if (!url.startsWith("https://script.google.com/")) {
    err = "URL ต้องขึ้นต้นด้วย https://script.google.com/";
    return false;
  }
  JsonDocument d;
  d["secret"] = secret;
  d["action"] = "ping";
  String body, resp;
  serializeJson(d, body);
  if (!gas::post(url, body, resp)) {
    err = "ติดต่อไม่ได้ (URL ถูกไหม? deploy เป็น web app หรือยัง?)";
    return false;
  }
  return okResponse(resp, err);
}

String status() {
  String s = "รอ sync: " + String((unsigned)store::pending().size()) + " ไฟล์";
  if (lastErr.length()) s += "\nsync ล่าสุดพลาด: " + lastErr;
  else if (lastOk) {
    struct tm lt;
    localtime_r(&lastOk, &lt);
    char buf[16];
    strftime(buf, sizeof buf, "%d/%m %H:%M", &lt);
    s += "\nsync สำเร็จล่าสุด: " + String(buf);
  }
  return s;
}

}  // namespace backup
