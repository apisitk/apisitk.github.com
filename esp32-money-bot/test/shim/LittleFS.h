// LittleFS on top of a host directory (FS_ROOT).
#pragma once
#include "Arduino.h"
#include <filesystem>
#include <vector>

extern std::string FS_ROOT;

class File : public Stream {
 public:
  FILE *f = nullptr;
  bool dir = false;
  std::string p;                  // full path inside the FS
  std::vector<std::string> list;  // directory entries (full FS paths)
  size_t next = 0;
  File() {}
  explicit File(FILE *fp, const std::string &path = "") : f(fp), p(path) {}
  explicit operator bool() const { return f || dir; }
  String path() const { return p; }
  File openNextFile() {
    if (!dir || next >= list.size()) return File();
    File x(nullptr, list[next++]);
    x.dir = true;  // only used for path(); never read
    return x;
  }
  size_t write(uint8_t c) override { return fputc(c, f) == EOF ? 0 : 1; }
  int available() override { return peek() >= 0; }
  int read() override { int c = fgetc(f); return c == EOF ? -1 : c; }
  int peek() override { int c = fgetc(f); if (c != EOF) ungetc(c, f); return c == EOF ? -1 : c; }
  void close() { if (f) fclose(f); f = nullptr; }
  ~File() {}
};

struct LittleFSShim {
  std::string p(const String &x) { return FS_ROOT + x.s; }
  bool begin(bool) { std::filesystem::create_directories(FS_ROOT); return true; }
  bool exists(const String &x) { return std::filesystem::exists(p(x)); }
  File open(const String &x) {  // directory
    File d;
    if (!std::filesystem::is_directory(p(x))) return d;
    d.dir = true;
    d.p = x.s;
    for (auto &e : std::filesystem::directory_iterator(p(x))) d.list.push_back(x.s + "/" + e.path().filename().string());
    std::sort(d.list.begin(), d.list.end());
    return d;
  }
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
