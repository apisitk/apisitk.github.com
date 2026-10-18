#pragma once
#include "config.h"
#include <Arduino.h>
#include <deque>
#include <vector>

// Users, accounts and transactions, kept on LittleFS.
//
//   /db.json                      users + accounts
//   /tx/<account id>/<YYYYMM>.jsonl  one transaction per line
//
// Account visibility: a user sees an account only if they are in its members.
// Private accounts have exactly one member and can never be shared.
// Users and accounts live in deques so pointers stay valid when adding more.

struct User {
  int64_t id;
  String name;
  String cur;  // id of the account in use
};

struct Category {
  String name;
  std::vector<String> kw;  // note containing any of these -> this category
};

struct Account {
  String id;   // internal, unique; also the directory name
  String key;  // what users type, e.g. "shop", "me"
  String name; // display name
  bool priv;
  std::vector<int64_t> members;
  std::vector<Category> cats;
  bool has(int64_t uid) const;
  Category *cat(const String &name);  // case-insensitive
  String autoCat(const String &note) const;  // "" if nothing matches
};

struct Tx {
  uint32_t id = 0;
  uint32_t ts = 0;    // unix time
  int32_t amt = 0;    // satang; >0 income, <0 expense
  String note;
  String cat;  // "" = uncategorized
  int64_t by = 0;
  bool del = false;
  bool synced = false;
};

struct CatSum {
  String cat;
  int64_t amt;  // satang, positive
};

struct Totals {
  int64_t in = 0, out = 0;  // satang, out is positive
  uint32_t count = 0;
  std::vector<CatSum> inCats, outCats;  // largest first
};

namespace store {

bool begin();  // mount FS and load db; false if FS failed
bool save();

std::deque<User> &users();
User *user(int64_t id);
User *userByName(const String &name);  // case-insensitive
User &addUser(int64_t id, const String &name);  // also creates (or reattaches) private "me"
// Drops the user and their shared memberships. Their private accounts stay
// on disk, invisible, and come back if the same chat id is added again.
bool removeUser(int64_t id, String &err);

Account *account(const String &id);
Account *resolve(int64_t uid, const String &key);  // only accounts uid can see
std::vector<Account *> visible(int64_t uid);
Account *create(int64_t owner, const String &key, const String &name, bool priv, String &err);
bool share(Account &a, int64_t uid, String &err);
bool unshare(Account &a, int64_t uid, String &err);

uint32_t addTx(const Account &a, int32_t amt, const String &note, const String &cat, int64_t by);
bool findTx(const Account &a, uint32_t id, Tx &out);  // live tx only
bool delTx(const Account &a, uint32_t id, Tx &removed);
// Replaces amt/note/cat of a live tx; `before` gets the old values.
bool editTx(const Account &a, uint32_t id, const Tx &changed, Tx &before);
bool lastBy(const Account &a, int64_t by, Tx &out);  // newest live tx by user
std::vector<Tx> recent(const Account &a, size_t n);  // newest first
Totals totals(const Account &a, time_t from, time_t to);

size_t fsUsed();
size_t fsTotal();

}  // namespace store
