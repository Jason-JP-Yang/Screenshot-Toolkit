// A small JSON reader, enough for GitHub's answers. Strings are kept as UTF-8.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sstk {

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Type type = Type::kNull;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Json> items;        // An array's values, or an object's values in the order of `keys`.
  std::vector<std::string> keys;  // An object's member names.

  const Json* Get(std::string_view key) const;
  std::string_view String(std::string_view key) const;  // Empty when absent or not a string.
  double Number(std::string_view key, double fallback) const;
  bool Bool(std::string_view key, bool fallback) const;
};

bool ParseJson(std::string_view source, Json* out);

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);

}  // namespace sstk
