#include "sstk/update/updater.h"

#include "sstk/base/product.h"
#include "sstk/base/settings.h"
#include "sstk/config/core.h"
#include "sstk/update/json.h"
#include "sstk/update/state.h"
#include "sstk/version.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>

namespace sstk {

namespace {

constexpr ULONGLONG kFirstCheckDelayMs = 60000;  // The network needs a moment after Windows starts.
constexpr ULONGLONG kStartupDelayMs = 30000;     // From the application starting to its check.
constexpr uint64_t kStartupSpacing = 600;        // Starts closer together than this share a check. Seconds.
constexpr ULONGLONG kCleanupRetryMs = 5000;
constexpr int kCleanupTries = 120;
constexpr size_t kReleasesLimit = 4u << 20;
constexpr char kMarker[] = "<!-- sstk-update ";
constexpr wchar_t kApiHeaders[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";

uint64_t UnixNow() {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  return (ticks - 116444736000000000ull) / 10000000ull;
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower;
}

struct Candidate {
  uint64_t build = 0;
  std::wstring name;
  std::wstring url;
  std::string sha256;
  uint64_t size = 0;
};

// The release with the highest build number, as its notes' hidden line gives it.
Candidate FindNewest(const Json& releases) {
  Candidate best;
  for (const Json& release : releases.items) {
    if (release.Bool("draft", false)) continue;
    const std::string_view body = release.String("body");
    const size_t start = body.find(kMarker);
    if (start == std::string_view::npos) continue;
    const size_t open = start + std::strlen(kMarker);
    const size_t end = body.find("-->", open);
    Json marker;
    if (end == std::string_view::npos || !ParseJson(body.substr(open, end - open), &marker)) continue;
    const double build = marker.Number("build", 0.0);
    if (build <= static_cast<double>(best.build)) continue;
    const std::string_view asset = marker.String("asset");
    const Json* assets = release.Get("assets");
    if (asset.empty() || assets == nullptr) continue;
    for (const Json& item : assets->items) {
      if (item.String("name") != asset) continue;
      best.build = static_cast<uint64_t>(build);
      best.name = Widen(marker.String("name"));
      best.url = Widen(item.String("browser_download_url"));
      best.sha256 = Lower(marker.String("sha256"));
      best.size = static_cast<uint64_t>(item.Number("size", 0.0));
      break;
    }
  }
  return best;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

Updater::~Updater() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  if (done_ != nullptr) CloseHandle(done_);
}

void Updater::Begin(ULONGLONG now) {
  done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  CreateUpdateKey();
  DeleteFileW((InstalledExecutablePath() + L".new").c_str());
  DeleteFileW((UpdateDirectory() + L"\\download.partial").c_str());

  UpdateStatus status;
  ReadUpdateStatus(&status);
  const std::wstring staged = StagedExecutablePath();
  // A download still newer than this build, unchanged since it was checked, goes on waiting. Anything else there is
  // left over from an update already in, or one that failed, and goes.
  if (Exists(staged) && status.state != UpdateState::kFailed &&
      ReadUpdateNumber(L"StagedBuild") > static_cast<uint64_t>(SSTK_BUILD_NUMBER) &&
      FileSha256(staged) == Narrow(ReadUpdateText(L"StagedSha256"))) {
    ready_ = true;
    WriteUpdateState(UpdateState::kReady, ReadUpdateText(L"StagedName"));
  } else {
    if (Exists(UpdateDirectory())) cleanup_ = now;
    if (status.state == UpdateState::kChecking || status.state == UpdateState::kDownloading || status.state == UpdateState::kReady ||
        status.state == UpdateState::kInstalling) {
      WriteUpdateState(UpdateState::kIdle, {});
    }
  }
  Schedule(now);
  if (due_ != 0) due_ = std::max(due_, now + kFirstCheckDelayMs);
}

void Updater::Schedule(ULONGLONG now) {
  due_ = 0;
  if (!ReadSettings().auto_update) return;
  const uint64_t wall = UnixNow();
  const uint64_t success = ReadUpdateNumber(L"LastSuccess");
  const uint64_t failure = ReadUpdateNumber(L"LastFailure");
  const uint64_t at = failure > success ? failure + config::kUpdateRetryMinutes * 60ull : success + config::kUpdateCheckHours * 3600ull;
  due_ = now + (at > wall ? (at - wall) * 1000ull : 0ull);
}

ULONGLONG Updater::NextWake(ULONGLONG now) const {
  ULONGLONG next = ULLONG_MAX;
  if (!running_ && !applying_) {
    if (due_ != 0) next = std::min(next, due_);
    if (soon_ != 0) next = std::min(next, soon_);
  }
  if (cleanup_ != 0) next = std::min(next, cleanup_);
  if (next == ULLONG_MAX) return INFINITE;
  return next > now ? next - now : 0;
}

void Updater::Check(bool manual) {
  if (manual) manual_ = true;
  if (running_ || applying_) return;
  if (worker_.joinable()) worker_.join();
  cleanup_ = 0;
  soon_ = 0;
  running_ = true;
  ResetEvent(done_);
  worker_ = std::thread([this] {
    outcome_ = Work();
    SetEvent(done_);
  });
}

void Updater::CheckSoon(ULONGLONG now) {
  if (running_ || applying_ || soon_ != 0 || !ReadSettings().auto_update) return;
  if (UnixNow() - ReadUpdateNumber(L"LastAttempt") < kStartupSpacing) return;
  soon_ = now + kStartupDelayMs;
}

void Updater::Tick(ULONGLONG now) {
  if (cleanup_ != 0 && now >= cleanup_) {
    if (DeleteTree(UpdateDirectory()) || ++cleanup_tries_ >= kCleanupTries) {
      cleanup_ = 0;
      cleanup_tries_ = 0;
    } else {
      cleanup_ = now + kCleanupRetryMs;
    }
  }
  if (!running_ && !applying_ && ((due_ != 0 && now >= due_) || (soon_ != 0 && now >= soon_))) Check(false);
}

void Updater::Finish(ULONGLONG now) {
  if (worker_.joinable()) worker_.join();
  ResetEvent(done_);
  running_ = false;
  const uint64_t wall = UnixNow();
  if (outcome_ == Outcome::kFailed) {
    WriteUpdateNumber(L"LastFailure", wall);
    WriteUpdateState(UpdateState::kFailed, {}, message_);
  } else {
    WriteUpdateNumber(L"LastSuccess", wall);
    WriteUpdateNumber(L"LastFailure", 0);
  }
  ready_ = outcome_ == Outcome::kReady;
  if (!ready_) manual_ = false;
  Schedule(now);
}

bool Updater::Apply() {
  if (!ready_ || applying_ || running_) return false;
  const std::wstring staged = StagedExecutablePath();
  const std::wstring system = SystemDirectory();
  std::wstring command_line = L"\"" + staged + L"\" --apply-update";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(staged.c_str(), command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system.c_str(), &startup,
                      &process)) {
    ready_ = false;
    manual_ = false;
    WriteUpdateState(UpdateState::kFailed, ReadUpdateText(L"StagedName"), L"The update couldn't start.");
    cleanup_ = GetTickCount64();
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  applying_ = true;
  return true;
}

Updater::Outcome Updater::Work() {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  WriteUpdateNumber(L"LastAttempt", UnixNow());
  WriteUpdateState(UpdateState::kChecking, {});

  const std::wstring api = std::wstring(L"https://api.github.com/repos/") + kRepository + L"/releases?per_page=30";
  std::string body;
  bool reached = false;
  for (const ProxyChoice& choice : ProxyChoices()) {
    if (stop_) break;
    HttpClient client(choice);
    if (client.Get(api, kApiHeaders, &body, kReleasesLimit)) {
      route_ = choice;
      reached = true;
      break;
    }
  }
  if (!reached) {
    message_ = L"Couldn't reach GitHub.";
    return Outcome::kFailed;
  }
  Json releases;
  if (!ParseJson(body, &releases) || releases.type != Json::Type::kArray) {
    message_ = L"GitHub's answer couldn't be read.";
    return Outcome::kFailed;
  }
  const Candidate best = FindNewest(releases);
  if (best.build <= static_cast<uint64_t>(SSTK_BUILD_NUMBER) || best.url.empty() || best.sha256.size() != 64) {
    WriteUpdateState(UpdateState::kUpToDate, SSTK_VERSION_WIDE);
    return Outcome::kUpToDate;
  }

  const std::wstring staged = StagedExecutablePath();
  if (ReadUpdateNumber(L"StagedBuild") == best.build && FileSha256(staged) == best.sha256) {
    WriteUpdateState(UpdateState::kReady, best.name);
    return Outcome::kReady;
  }

  WriteUpdateState(UpdateState::kDownloading, best.name);
  const std::wstring directory = UpdateDirectory();
  CreateDirectoryW(directory.c_str(), nullptr);
  // Written under a name nothing reads, so a download cut off at any point is never taken for a finished one.
  const std::wstring partial = directory + L"\\download.partial";
  HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    message_ = L"Couldn't save the download.";
    return Outcome::kFailed;
  }
  HttpClient client(route_);
  Sha256 hash;
  uint64_t written = 0;
  DWORD shown = 0;
  const bool fetched = client.Download(best.url, file, &hash, &written, [&](uint64_t bytes) {
    if (best.size > 0) {
      const DWORD progress = static_cast<DWORD>(std::min<uint64_t>(bytes * 1000 / best.size, 1000));
      if (progress >= shown + 10 || progress == 1000) {
        shown = progress;
        WriteUpdateProgress(progress);
      }
    }
    return !stop_.load();
  });
  const bool flushed = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!fetched || !flushed) {
    DeleteFileW(partial.c_str());
    message_ = L"The download failed.";
    return Outcome::kFailed;
  }
  if ((best.size > 0 && written != best.size) || hash.Finish() != best.sha256) {
    DeleteFileW(partial.c_str());
    message_ = L"The download didn't match the release.";
    return Outcome::kFailed;
  }
  if (!MoveFileExW(partial.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(partial.c_str());
    message_ = L"Couldn't save the download.";
    return Outcome::kFailed;
  }
  WriteUpdateNumber(L"StagedBuild", best.build);
  WriteUpdateText(L"StagedName", best.name);
  WriteUpdateText(L"StagedSha256", Widen(best.sha256));
  WriteUpdateState(UpdateState::kReady, best.name);
  return Outcome::kReady;
}

}  // namespace sstk
