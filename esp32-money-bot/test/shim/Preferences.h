#pragma once
#include <cstdint>
#include <map>
#include <string>
class Preferences {
  std::map<std::string, int64_t> m;
 public:
  void begin(const char *, bool) {}
  uint32_t getUInt(const char *k, uint32_t d) { return m.count(k) ? m[k] : d; }
  void putUInt(const char *k, uint32_t v) { m[k] = v; }
};
