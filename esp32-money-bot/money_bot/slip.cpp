#include "slip.h"
#include "gas.h"
#include <ArduinoJson.h>

namespace slip {

static String sharedUrl, sharedSecret;

void begin(const char *url, const char *secret) {
  sharedUrl = url;
  sharedSecret = secret;
}

bool enabled(const User &u) { return sharedUrl.length() || u.bkUrl.length(); }

// One attempt against one web app. `notSet` = that script has no Typhoon
// key / bot token configured, so the caller may try the next one.
static bool readFrom(const String &url, const String &secret, const String &fileId, Info &out, String &err,
                     bool &notSet) {
  notSet = false;
  JsonDocument req;
  req["secret"] = secret;
  req["action"] = "slip";
  req["file_id"] = fileId;
  String body, resp;
  serializeJson(req, body);
  if (!gas::post(url, body, resp, SLIP_TIMEOUT_MS)) {
    err = "ติดต่อ Apps Script ไม่ได้";
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, resp)) {
    err = "Apps Script ตอบกลับไม่ใช่ JSON";
    return false;
  }
  if (!(d["ok"] | false)) {
    err = d["error"] | "อ่านไม่ได้";
    notSet = err.indexOf("not set") >= 0;
    return false;
  }
  JsonObject s = d["slip"];
  out.amt = s["amt"] | 0;
  out.ts = s["ts"] | 0;
  out.from = s["from"] | "";
  out.fromBank = s["from_bank"] | "";
  out.to = s["to"] | "";
  out.toBank = s["to_bank"] | "";
  out.ref = s["ref"] | "";
  out.memo = s["memo"] | "";
  if (out.amt <= 0) {
    err = "หาจำนวนเงินในรูปไม่เจอ (ใช่สลิปไหม?)";
    return false;
  }
  return true;
}

bool read(const User &u, const String &fileId, Info &out, String &err) {
  bool notSet = false;
  if (u.bkUrl.length()) {
    if (readFrom(u.bkUrl, u.bkSecret, fileId, out, err, notSet)) return true;
    if (!notSet) return false;  // a real error; don't hand the slip to another script
  }
  if (!sharedUrl.length()) {
    err = "ยังไม่ได้ตั้งตัวอ่านสลิป — ใส่ TYPHOON_KEY และ BOT_TOKEN ใน Script Properties ของชีตคุณ (หรือ SLIP_URL ใน secrets.h)";
    return false;
  }
  return readFrom(sharedUrl, sharedSecret, fileId, out, err, notSet);
}

// Lowercase, trimmed, without a leading title ("นาย", "MR." ...).
static String stripTitle(const String &in) {
  static const char *titles[] = {"นางสาว", "น.ส.", "นาย", "นาง", "ด.ช.", "ด.ญ.", "mrs.", "mrs ", "mr.", "mr ", "ms.", "ms ", "miss "};
  String s = in;
  s.toLowerCase();
  s.trim();
  for (const char *t : titles)
    if (s.startsWith(t)) {
      s = s.substring(strlen(t));
      break;
    }
  s.trim();
  return s;
}

// Without spaces, dots and slip masking ("*").
static String normalize(const String &in) {
  String out;
  for (char c : stripTitle(in))
    if (c != ' ' && c != '.' && c != '*') out += c;
  return out;
}

// "นาย สมชาย ใ***" -> "สมชาย"
static String firstName(const String &in) {
  String s = stripTitle(in);
  int sp = s.indexOf(' ');
  return sp > 0 ? s.substring(0, sp) : s;
}

bool nameMatch(const String &slipName, const String &mine) {
  String a = normalize(slipName), b = normalize(mine);
  if (a.length() < 2 || b.length() < 2) return false;
  if (a.indexOf(b) >= 0) return true;  // "ร้านป้าแดง สาขา 2" contains "ร้านป้าแดง"
  // Slips mask surnames ("สมชาย ใ***", "SOMCHAI J"), so the first name decides.
  String fa = firstName(slipName), fb = firstName(mine);
  return fa.length() >= 2 && fa == fb;
}

int direction(const Account &a, const Info &s) {
  bool toMe = false, fromMe = false;
  for (const String &n : a.myNames) {
    toMe |= nameMatch(s.to, n);
    fromMe |= nameMatch(s.from, n);
  }
  if (toMe && !fromMe) return 1;
  if (fromMe && !toMe) return -1;
  return 0;  // unknown, or a transfer between own accounts
}

}  // namespace slip
