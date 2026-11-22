// HTTPS requests for the update service, over WinHTTP.
//
// The service runs as SYSTEM, which has no proxy settings of its own, so it borrows those of the signed-in users: a
// proxy tool such as Clash sets them for the user only. TLS is checked against Windows' trusted roots whatever the
// proxy, so a proxy can route a download but never change it.
#pragma once

#include "sstk/base/platform.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sstk {

struct ProxyChoice {
  DWORD access = 0;     // A WINHTTP_ACCESS_TYPE_ value.
  std::wstring proxy;   // For a named proxy, as Windows stores it, such as "127.0.0.1:7890".
  std::wstring bypass;
  std::wstring script;  // A PAC address, when the user's settings name one.
};

// The signed-in users' proxy settings, in the order to try them, then Windows' own.
std::vector<ProxyChoice> ProxyChoices();

class Sha256 {
 public:
  Sha256();
  ~Sha256();
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void Add(const void* data, size_t size);
  std::string Finish();  // Lowercase hex.

 private:
  void* algorithm_ = nullptr;
  void* hash_ = nullptr;
};

// Empty when the file cannot be read.
std::string FileSha256(const std::wstring& path);

class HttpClient {
 public:
  explicit HttpClient(const ProxyChoice& choice);
  ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  // A GET answered with 200, read into memory up to `limit` bytes.
  bool Get(const std::wstring& url, const wchar_t* headers, std::string* body, size_t limit);

  // A GET answered with 200, streamed into `file` and hashed on the way. `keep_going` hears the bytes so far, and
  // stops the download by returning false.
  bool Download(const std::wstring& url, HANDLE file, Sha256* hash, uint64_t* written, const std::function<bool(uint64_t)>& keep_going);

 private:
  void* Open(const std::wstring& url, const wchar_t* headers, void** connection);

  void* session_ = nullptr;
  ProxyChoice choice_;
};

}  // namespace sstk
