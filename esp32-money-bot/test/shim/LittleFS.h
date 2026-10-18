// LittleFS on top of a host directory (FS_ROOT).
#pragma once
#include "Arduino.h"
#include <filesystem>

extern std::string FS_ROOT;

class File : public Stream {
 public:
  FILE *f = nullptr;
  File() {}
  explicit File(FILE *fp) : f(fp) {}
  explicit operator bool() const { return f; }
  size_t write(uint8_t c) override { return fputc(c, f) == EOF ? 0 : 1; }
  int available() override { return peek() >= 0; }
  int read() override { int c = fgetc(f); return c == EOF ? -1 : c; }
  int peek() override { int c = fgetc(f); if (c != EOF) ungetc(c, f); return c == EOF ? -1 : c; }
  void close() { if (f) fclose(f); f = nullptr; }
};

struct LittleFSShim {
  std::string p(const String &x) { return FS_ROOT + x.s; }
  bool begin(bool) { std::filesystem::create_directories(FS_ROOT); return true; }
  bool exists(const String &x) { return std::filesystem::exists(p(x)); }
  File open(const String &x, const char *mode, bool create = false) {
    if (create) std::filesystem::create_directories(std::filesystem::path(p(x)).parent_path());
    return File(fopen(p(x).c_str(), mode));
  }
  bool remove(const String &x) { return std::filesystem::remove(p(x)); }
  bool rename(const String &a, const String &b) { std::error_code e; std::filesystem::rename(p(a), p(b), e); return !e; }
  size_t usedBytes() { return 0; }
  size_t totalBytes() { return 1966080; }
};
extern LittleFSShim LittleFS;
