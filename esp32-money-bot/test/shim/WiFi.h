#pragma once
struct WiFiShim { int RSSI() { return -60; } };
extern WiFiShim WiFi;
