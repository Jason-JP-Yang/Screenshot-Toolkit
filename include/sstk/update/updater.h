// Automatic updates from the GitHub releases.
//
// The service checks at each start of the application and every 12 hours, and an hour after a check fails. Each
// release's notes carry a hidden line with its build number, the count of commits on main, and its file's SHA-256: a
// release is newer only when its build number is higher, so a copy built from a commit GitHub has not seen is never
// replaced. The download runs at background priority into update\ under the install folder, where only administrators
// and SYSTEM write, and takes its final name only once its size and hash match.
//
// It goes in when nothing is on screen and nobody has touched the keyboard or mouse for a minute, which the application
// reports, or at once when no application runs or the About window asked. The apply step then runs from the
// download: it stops the service, hands each application over to the new copy, which starts fully before the old one
// lets go, replaces the installed file in one rename, and starts the service again, which hands the applications back
// to the installed file and deletes the download. The installed file is whole at every moment, whatever is stopped.
#pragma once

#include "sstk/base/platform.h"
#include "sstk/update/http.h"

#include <atomic>
#include <string>
#include <thread>

namespace sstk {

// The service's half: when to check, the check and download on a worker thread, and starting the apply step.
class Updater {
 public:
  Updater() = default;
  ~Updater();
  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  void Begin(ULONGLONG now);
  HANDLE done() const { return done_; }  // Set when the worker ends; then call Finish.
  ULONGLONG NextWake(ULONGLONG now) const;  // Milliseconds until Tick has something to do, or INFINITE.

  void Check(bool manual);
  void CheckSoon(ULONGLONG now);  // After the application starts.
  void Tick(ULONGLONG now);
  void Finish(ULONGLONG now);

  bool ready() const { return ready_ && !applying_; }
  bool manual() const { return manual_; }
  bool Apply();

 private:
  enum class Outcome { kFailed, kUpToDate, kReady };

  Outcome Work();
  void Schedule(ULONGLONG now);

  std::thread worker_;
  HANDLE done_ = nullptr;
  std::atomic<bool> stop_{false};
  Outcome outcome_ = Outcome::kFailed;
  std::wstring message_;
  ProxyChoice route_;
  bool running_ = false;
  bool manual_ = false;
  bool ready_ = false;
  bool applying_ = false;
  ULONGLONG due_ = 0;      // The next scheduled check, as a tick count; 0 for none.
  ULONGLONG soon_ = 0;     // A check after the application started.
  ULONGLONG cleanup_ = 0;  // The next try at deleting a download no longer needed.
  int cleanup_tries_ = 0;
};

// Runs as the apply step (--apply-update), from the downloaded copy, as SYSTEM.
int RunApplyUpdate();

}  // namespace sstk
