#include "sstk/update/http.h"

#include <bcrypt.h>
#include <sddl.h>
#include <winhttp.h>
#include <wtsapi32.h>

#include <algorithm>

namespace sstk {

namespace {

constexpr wchar_t kAgent[] = L"ScreenshotToolkit-Updater";
constexpr wchar_t kInternetSettings[] = L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
constexpr DWORD kChunk = 64 * 1024;

std::wstring ReadText(HKEY key, const wchar_t* name) {
  wchar_t buffer[2048];
  DWORD size = sizeof(buffer) - sizeof(wchar_t);
  DWORD type = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) != ERROR_SUCCESS ||
      (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }
  buffer[size / sizeof(wchar_t)] = L'\0';
  return buffer;
}

void AddUserChoices(DWORD session, std::vector<ProxyChoice>& choices) {
  HANDLE token = nullptr;
  if (!WTSQueryUserToken(session, &token)) return;
  BYTE buffer[256];
  DWORD size = 0;
  wchar_t* sid = nullptr;
  if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size) &&
      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, (std::wstring(sid) + kInternetSettings).c_str(), 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
      const std::wstring script = ReadText(key, L"AutoConfigURL");
      if (!script.empty()) choices.push_back({WINHTTP_ACCESS_TYPE_NO_PROXY, {}, {}, script});
      DWORD enabled = 0;
      DWORD length = sizeof(enabled);
      if (RegQueryValueExW(key, L"ProxyEnable", nullptr, nullptr, reinterpret_cast<BYTE*>(&enabled), &length) == ERROR_SUCCESS &&
          enabled != 0) {
        const std::wstring server = ReadText(key, L"ProxyServer");
        if (!server.empty()) choices.push_back({WINHTTP_ACCESS_TYPE_NAMED_PROXY, server, ReadText(key, L"ProxyOverride"), {}});
      }
      RegCloseKey(key);
    }
    LocalFree(sid);
  }
  CloseHandle(token);
}

}  // namespace

std::vector<ProxyChoice> ProxyChoices() {
  std::vector<ProxyChoice> choices;
  WTS_SESSION_INFOW* sessions = nullptr;
  DWORD count = 0;
  if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
    for (DWORD i = 0; i < count; ++i) {
      if (sessions[i].State == WTSActive) AddUserChoices(sessions[i].SessionId, choices);
    }
    WTSFreeMemory(sessions);
  }
  choices.push_back({WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, {}, {}, {}});
  return choices;
}

Sha256::Sha256() {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) hash = nullptr;
  }
  algorithm_ = algorithm;
  hash_ = hash;
}

Sha256::~Sha256() {
  if (hash_ != nullptr) BCryptDestroyHash(hash_);
  if (algorithm_ != nullptr) BCryptCloseAlgorithmProvider(algorithm_, 0);
}

void Sha256::Add(const void* data, size_t size) {
  if (hash_ != nullptr) BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
}

std::string Sha256::Finish() {
  UCHAR digest[32];
  if (hash_ == nullptr || !BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, sizeof(digest), 0))) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (const UCHAR byte : digest) {
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0xF]);
  }
  return text;
}

std::string FileSha256(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  Sha256 hash;
  std::vector<BYTE> buffer(kChunk);
  DWORD read = 0;
  bool ok = true;
  while ((ok = ReadFile(file, buffer.data(), kChunk, &read, nullptr) != FALSE) && read > 0) hash.Add(buffer.data(), read);
  CloseHandle(file);
  return ok ? hash.Finish() : std::string();
}

HttpClient::HttpClient(const ProxyChoice& choice) : choice_(choice) {
  const bool named = choice.access == WINHTTP_ACCESS_TYPE_NAMED_PROXY;
  session_ = WinHttpOpen(kAgent, choice.access, named ? choice.proxy.c_str() : WINHTTP_NO_PROXY_NAME,
                         named && !choice.bypass.empty() ? choice.bypass.c_str() : WINHTTP_NO_PROXY_BYPASS, 0);
  if (session_ == nullptr) return;
  WinHttpSetTimeouts(session_, 15000, 15000, 20000, 20000);
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
  protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
  if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
    protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
  }
}

HttpClient::~HttpClient() {
  if (session_ != nullptr) WinHttpCloseHandle(session_);
}

void* HttpClient::Open(const std::wstring& url, const wchar_t* headers, void** connection) {
  *connection = nullptr;
  if (session_ == nullptr) return nullptr;
  URL_COMPONENTS parts{sizeof(parts)};
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return nullptr;
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  *connection = WinHttpConnect(session_, host.c_str(), parts.nPort, 0);
  if (*connection == nullptr) return nullptr;
  HINTERNET request = WinHttpOpenRequest(*connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
  if (request == nullptr) return nullptr;
  if (!choice_.script.empty()) {
    WINHTTP_AUTOPROXY_OPTIONS options{};
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = choice_.script.c_str();
    options.fAutoLogonIfChallenged = TRUE;
    WINHTTP_PROXY_INFO info{};
    if (WinHttpGetProxyForUrl(session_, url.c_str(), &options, &info)) {
      WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &info, sizeof(info));
      if (info.lpszProxy != nullptr) GlobalFree(info.lpszProxy);
      if (info.lpszProxyBypass != nullptr) GlobalFree(info.lpszProxyBypass);
    }
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  const bool answered =
      WinHttpSendRequest(request, headers != nullptr ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, headers != nullptr ? static_cast<DWORD>(-1L) : 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr) &&
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                          WINHTTP_NO_HEADER_INDEX);
  if (!answered || status != 200) {
    WinHttpCloseHandle(request);
    return nullptr;
  }
  return request;
}

bool HttpClient::Get(const std::wstring& url, const wchar_t* headers, std::string* body, size_t limit) {
  body->clear();
  void* connection = nullptr;
  HINTERNET request = Open(url, headers, &connection);
  bool ok = request != nullptr;
  std::vector<char> buffer(kChunk);
  while (ok) {
    DWORD read = 0;
    if (!WinHttpReadData(request, buffer.data(), kChunk, &read)) ok = false;
    else if (read == 0) break;
    else if (body->size() + read > limit) ok = false;
    else body->append(buffer.data(), read);
  }
  if (request != nullptr) WinHttpCloseHandle(request);
  if (connection != nullptr) WinHttpCloseHandle(connection);
  return ok;
}

bool HttpClient::Download(const std::wstring& url, HANDLE file, Sha256* hash, uint64_t* written,
                          const std::function<bool(uint64_t)>& keep_going) {
  *written = 0;
  void* connection = nullptr;
  HINTERNET request = Open(url, L"Accept: application/octet-stream\r\n", &connection);
  bool ok = request != nullptr;
  std::vector<BYTE> buffer(kChunk);
  while (ok) {
    DWORD read = 0;
    DWORD stored = 0;
    if (!WinHttpReadData(request, buffer.data(), kChunk, &read)) ok = false;
    else if (read == 0) break;
    else if (!WriteFile(file, buffer.data(), read, &stored, nullptr) || stored != read) ok = false;
    else {
      hash->Add(buffer.data(), read);
      *written += read;
      ok = keep_going(*written);
    }
  }
  if (request != nullptr) WinHttpCloseHandle(request);
  if (connection != nullptr) WinHttpCloseHandle(connection);
  return ok;
}

}  // namespace sstk
