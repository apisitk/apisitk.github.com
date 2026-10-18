#include "tg.h"
#include "ca_bundle.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace tg {

static WiFiClientSecure client;
static String base;

void begin(const char *token) {
  base = String("https://api.telegram.org/bot") + token + "/";
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  client.setCACertBundle(CA_BUNDLE, CA_BUNDLE_LEN);
#else
  client.setCACertBundle(CA_BUNDLE);
#endif
}

// POST a JSON body, return HTTP status (or negative on transport error).
static int post(const char *method, const String &body, String &resp, int timeoutMs) {
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, base + method)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code > 0) resp = http.getString();
  http.end();
  return code;
}

bool poll(int64_t offset, int timeoutSec, std::vector<Update> &out) {
  JsonDocument req;
  req["offset"] = offset;
  req["timeout"] = timeoutSec;
  req["limit"] = POLL_LIMIT;
  req["allowed_updates"][0] = "message";
  req["allowed_updates"][1] = "callback_query";
  String body, resp;
  serializeJson(req, body);

  int code = post("getUpdates", body, resp, (timeoutSec + 10) * 1000);
  if (code != 200) {
    Serial.printf("[tg] getUpdates http %d\n", code);
    return false;
  }

  // Keep only the fields we use; saves a lot of RAM on big updates.
  JsonDocument filter;
  JsonObject f = filter["result"][0].to<JsonObject>();
  f["update_id"] = true;
  f["message"]["chat"]["id"] = true;
  f["message"]["chat"]["type"] = true;
  f["message"]["from"]["id"] = true;
  f["message"]["from"]["first_name"] = true;
  f["message"]["text"] = true;
  f["message"]["caption"] = true;
  f["message"]["photo"][0]["file_id"] = true;
  f["message"]["photo"][0]["file_size"] = true;
  f["message"]["document"]["file_id"] = true;
  f["message"]["document"]["mime_type"] = true;
  f["callback_query"]["message"]["chat"]["id"] = true;
  f["callback_query"]["message"]["chat"]["type"] = true;
  f["callback_query"]["message"]["message_id"] = true;
  f["callback_query"]["id"] = true;
  f["callback_query"]["data"] = true;
  f["callback_query"]["from"]["id"] = true;
  f["callback_query"]["from"]["first_name"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) return false;

  for (JsonObject u : doc["result"].as<JsonArray>()) {
    Update up;
    up.updateId = u["update_id"].as<int64_t>();
    JsonObject cb = u["callback_query"];
    bool isCb = !cb.isNull();
    JsonObject m = isCb ? cb["message"].as<JsonObject>() : u["message"].as<JsonObject>();
    JsonObject from = isCb ? cb["from"].as<JsonObject>() : m["from"].as<JsonObject>();
    up.chatId = m["chat"]["id"].as<int64_t>();
    up.isPrivate = m["chat"]["type"] == "private";
    up.fromId = from["id"].as<int64_t>();
    up.fromName = from["first_name"] | "";
    if (isCb) {
      up.cbId = cb["id"] | "";
      up.cbData = cb["data"] | "";
      up.cbMsgId = m["message_id"] | 0;
    } else {
      up.text = m["text"] | "";  // empty for photos, stickers, ...
      up.caption = m["caption"] | "";
      // Telegram keeps each photo in several sizes, smallest first. Read the
      // largest; keep the largest one that fits the size limit.
      JsonArray photo = m["photo"];
      for (JsonObject ps : photo) {
        up.photoId = ps["file_id"] | "";
        if ((ps["file_size"] | 0) <= SLIP_KEEP_MAX_BYTES || !up.keepId.length()) up.keepId = up.photoId;
      }
      String mime = m["document"]["mime_type"] | "";
      if (mime.startsWith("image/")) up.photoId = up.keepId = m["document"]["file_id"] | "";  // one size only
    }
    out.push_back(up);
  }
  return true;
}

static void addKeyboard(JsonDocument &req, const String &keyboard) {
  if (keyboard.length()) req["reply_markup"]["inline_keyboard"] = serialized(keyboard);
}

int32_t send(int64_t chatId, const String &text, const String &keyboard) {
  JsonDocument req;
  req["chat_id"] = chatId;
  req["text"] = text;  // plain text: no parse_mode, so no escaping needed
  addKeyboard(req, keyboard);
  String body, resp;
  serializeJson(req, body);
  int code = post("sendMessage", body, resp, 15000);
  if (code != 200) {
    Serial.printf("[tg] sendMessage http %d %s\n", code, resp.c_str());
    return 0;
  }
  JsonDocument filter, doc;
  filter["result"]["message_id"] = true;
  deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  return doc["result"]["message_id"] | 1;  // 1: sent, id unknown
}

bool edit(int64_t chatId, int32_t msgId, const String &text, const String &keyboard) {
  JsonDocument req;
  req["chat_id"] = chatId;
  req["message_id"] = msgId;
  req["text"] = text;
  addKeyboard(req, keyboard);  // no keyboard given = buttons removed
  String body, resp;
  serializeJson(req, body);
  int code = post("editMessageText", body, resp, 15000);
  if (code != 200) Serial.printf("[tg] editMessageText http %d %s\n", code, resp.c_str());
  return code == 200;
}

void answer(const String &cbId, const String &text) {
  JsonDocument req;
  req["callback_query_id"] = cbId;
  if (text.length()) req["text"] = text;
  String body, resp;
  serializeJson(req, body);
  post("answerCallbackQuery", body, resp, 10000);
}

}  // namespace tg
