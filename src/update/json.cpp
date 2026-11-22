#include "sstk/update/json.h"

#include "sstk/base/platform.h"

#include <cstdlib>
#include <cstring>

namespace sstk {

namespace {

constexpr int kMaxDepth = 64;

void AppendUtf8(std::string* out, unsigned code) {
  if (code < 0x80) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view source) : s_(source) {}

  bool Parse(Json* out) {
    if (!Value(out, 0)) return false;
    Skip();
    return at_ == s_.size();
  }

 private:
  void Skip() {
    while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) ++at_;
  }

  bool Literal(std::string_view word) {
    if (s_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }

  bool Value(Json* out, int depth) {
    if (depth > kMaxDepth) return false;
    Skip();
    if (at_ >= s_.size()) return false;
    switch (s_[at_]) {
      case '{': return Object(out, depth);
      case '[': return Array(out, depth);
      case '"': out->type = Json::Type::kString; return String(&out->text);
      case 't': out->type = Json::Type::kBool; out->boolean = true; return Literal("true");
      case 'f': out->type = Json::Type::kBool; out->boolean = false; return Literal("false");
      case 'n': out->type = Json::Type::kNull; return Literal("null");
      default: return Number(out);
    }
  }

  bool Number(Json* out) {
    const size_t start = at_;
    while (at_ < s_.size() && s_[at_] != '\0' && std::strchr("+-0123456789.eE", s_[at_]) != nullptr) ++at_;
    if (at_ == start) return false;
    const std::string digits(s_.substr(start, at_ - start));
    char* end = nullptr;
    out->type = Json::Type::kNumber;
    out->number = std::strtod(digits.c_str(), &end);
    return end == digits.c_str() + digits.size();
  }

  bool Hex4(unsigned* code) {
    if (at_ + 4 > s_.size()) return false;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s_[at_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
      else return false;
    }
    *code = value;
    return true;
  }

  bool String(std::string* out) {
    ++at_;
    out->clear();
    while (at_ < s_.size()) {
      const char c = s_[at_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (at_ >= s_.size()) return false;
      switch (const char escape = s_[at_++]) {
        case '"': case '\\': case '/': out->push_back(escape); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          if (!Hex4(&code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF && at_ + 1 < s_.size() && s_[at_] == '\\' && s_[at_ + 1] == 'u') {
            at_ += 2;
            unsigned low = 0;
            if (!Hex4(&low)) return false;
            code = low >= 0xDC00 && low <= 0xDFFF ? 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
          } else if (code >= 0xD800 && code <= 0xDFFF) {
            code = 0xFFFD;
          }
          AppendUtf8(out, code);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool Array(Json* out, int depth) {
    out->type = Json::Type::kArray;
    ++at_;
    Skip();
    if (at_ < s_.size() && s_[at_] == ']') {
      ++at_;
      return true;
    }
    for (;;) {
      out->items.emplace_back();
      if (!Value(&out->items.back(), depth + 1)) return false;
      Skip();
      if (at_ >= s_.size()) return false;
      if (s_[at_] == ',') {
        ++at_;
        continue;
      }
      if (s_[at_] != ']') return false;
      ++at_;
      return true;
    }
  }

  bool Object(Json* out, int depth) {
    out->type = Json::Type::kObject;
    ++at_;
    Skip();
    if (at_ < s_.size() && s_[at_] == '}') {
      ++at_;
      return true;
    }
    for (;;) {
      Skip();
      if (at_ >= s_.size() || s_[at_] != '"') return false;
      out->keys.emplace_back();
      if (!String(&out->keys.back())) return false;
      Skip();
      if (at_ >= s_.size() || s_[at_] != ':') return false;
      ++at_;
      out->items.emplace_back();
      if (!Value(&out->items.back(), depth + 1)) return false;
      Skip();
      if (at_ >= s_.size()) return false;
      if (s_[at_] == ',') {
        ++at_;
        continue;
      }
      if (s_[at_] != '}') return false;
      ++at_;
      return true;
    }
  }

  std::string_view s_;
  size_t at_ = 0;
};

}  // namespace

const Json* Json::Get(std::string_view key) const {
  if (type != Type::kObject) return nullptr;
  for (size_t i = 0; i < keys.size() && i < items.size(); ++i) {
    if (keys[i] == key) return &items[i];
  }
  return nullptr;
}

std::string_view Json::String(std::string_view key) const {
  const Json* value = Get(key);
  return value != nullptr && value->type == Type::kString ? std::string_view(value->text) : std::string_view();
}

double Json::Number(std::string_view key, double fallback) const {
  const Json* value = Get(key);
  return value != nullptr && value->type == Type::kNumber ? value->number : fallback;
}

bool Json::Bool(std::string_view key, bool fallback) const {
  const Json* value = Get(key);
  return value != nullptr && value->type == Type::kBool ? value->boolean : fallback;
}

bool ParseJson(std::string_view source, Json* out) {
  *out = Json{};
  return Parser(source).Parse(out);
}

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
  return wide;
}

std::string Narrow(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
  return utf8;
}

}  // namespace sstk
