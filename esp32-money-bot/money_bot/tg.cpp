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

  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) return false;

  for (JsonObject u : doc["result"].as<JsonArray>()) {
    Update up;
    up.updateId = u["update_id"].as<int64_t>();
    JsonObject m = u["message"];
    up.chatId = m["chat"]["id"].as<int64_t>();
    up.isPrivate = m["chat"]["type"] == "private";
    up.fromId = m["from"]["id"].as<int64_t>();
    up.fromName = m["from"]["first_name"] | "";
    up.text = m["text"] | "";  // empty for photos, stickers, ...
    out.push_back(up);
  }
  return true;
}

bool send(int64_t chatId, const String &text) {
  JsonDocument req;
  req["chat_id"] = chatId;
  req["text"] = text;  // plain text: no parse_mode, so no escaping needed
  String body, resp;
  serializeJson(req, body);
  int code = post("sendMessage", body, resp, 15000);
  if (code != 200) Serial.printf("[tg] sendMessage http %d %s\n", code, resp.c_str());
  return code == 200;
}

}  // namespace tg
