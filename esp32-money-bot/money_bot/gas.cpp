#include "gas.h"
#include "ca_bundle.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace gas {

bool post(const String &url, const String &body, String &resp, uint16_t timeoutMs) {
  // Own TLS client, closed after each call: Google is called rarely, and
  // keeping it open would hold ~40KB of RAM next to the Telegram one.
  WiFiClientSecure client;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  client.setCACertBundle(CA_BUNDLE, CA_BUNDLE_LEN);
#else
  client.setCACertBundle(CA_BUNDLE);
#endif
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code > 0) resp = http.getString();
  http.end();
  if (code != 200) Serial.printf("[gas] http %d\n", code);
  return code == 200;
}

}  // namespace gas
