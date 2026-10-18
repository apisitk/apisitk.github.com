// Host stand-ins for the bits of the Arduino core the bot uses, so the real
// bot.cpp / store.cpp can run on a PC. Not a general Arduino emulation.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>

#define ARDUINO 10800
#define ARDUINOJSON_ENABLE_PROGMEM 0

class String {
 public:
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(const std::string &v) : s(v) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  const char *c_str() const { return s.c_str(); }
  unsigned length() const { return s.size(); }
  char operator[](unsigned i) const { return i < s.size() ? s[i] : 0; }
  char &operator[](unsigned i) { return s[i]; }
  String substring(unsigned a) const { return a < s.size() ? s.substr(a) : ""; }
  String substring(unsigned a, unsigned b) const { return a < s.size() && b > a ? s.substr(a, b - a) : ""; }
  int indexOf(char c, unsigned from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const String &t, unsigned from = 0) const { auto p = s.find(t.s, from); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const String &p) const { return s.compare(0, p.s.size(), p.s) == 0; }
  bool endsWith(const String &p) const { return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0; }
  void trim() {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
  }
  void toLowerCase() { for (char &c : s) c = tolower((unsigned char)c); }
  bool equalsIgnoreCase(const String &o) const { return strcasecmp(s.c_str(), o.s.c_str()) == 0; }
  bool concat(const char *c) { s += c; return true; }
  bool concat(const char *c, size_t n) { s.append(c, n); return true; }
  bool concat(char c) { s += c; return true; }
  bool reserve(unsigned n) { s.reserve(n); return true; }
  String &operator+=(const String &o) { s += o.s; return *this; }
  String &operator+=(const char *o) { s += o; return *this; }
  String &operator+=(char o) { s += o; return *this; }
  bool operator==(const String &o) const { return s == o.s; }
  bool operator==(const char *o) const { return s == o; }
  bool operator!=(const String &o) const { return s != o.s; }
  std::string::const_iterator begin() const { return s.begin(); }
  std::string::const_iterator end() const { return s.end(); }
};
inline String operator+(const String &a, const String &b) { return a.s + b.s; }
inline String operator+(const String &a, const char *b) { return a.s + b; }
inline String operator+(const char *a, const String &b) { return std::string(a) + b.s; }
inline String operator+(const String &a, char b) { return a.s + b; }

class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t *b, size_t n) { size_t k = 0; while (n--) k += write(*b++); return k; }
  size_t println(const String &v) { return write((const uint8_t *)v.c_str(), v.length()) + write('\n'); }
};
class Printable {
 public:
  virtual size_t printTo(Print &p) const = 0;
};
class Stream : public Print {
 public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  size_t readBytes(char *b, size_t n) { size_t k = 0; int c; while (k < n && (c = read()) >= 0) b[k++] = c; return k; }
  String readStringUntil(char t) { std::string r; int c; while ((c = read()) >= 0 && c != t) r += (char)c; return r; }
};

struct SerialShim { template <typename... A> void printf(const char *f, A... a) { ::printf(f, a...); } void println(const char *s) { puts(s); } };
extern SerialShim Serial;
inline unsigned long millis() { return clock() / (CLOCKS_PER_SEC / 1000); }
#include <algorithm>
using std::min;
template <typename T> T constrain(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
struct EspShim { unsigned getFreeHeap() { return 200000; } };
extern EspShim ESP;
