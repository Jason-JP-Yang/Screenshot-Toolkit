// Where an update stands. The service and the apply step write it to HKLM\SOFTWARE\Screenshot Toolkit\Update as
// SYSTEM; the application and the About window only read it, and a change wakes the application through the
// registry's own notification, so watching costs nothing.
#pragma once

#include "sstk/base/platform.h"

#include <cstdint>
#include <string>

namespace sstk {

enum class UpdateState : DWORD {
  kIdle,
  kChecking,
  kDownloading,
  kReady,       // Downloaded and verified, waiting for a quiet moment.
  kInstalling,
  kUpToDate,
  kUpdated,
  kFailed,
};

struct UpdateStatus {
  UpdateState state = UpdateState::kIdle;
  DWORD sequence = 0;    // Grows with every change of state.
  DWORD progress = 0;    // Of a download, in thousandths.
  std::wstring target;   // The build being fetched or installed, or the one installed last.
  std::wstring message;  // Why it failed.
};

std::wstring UpdateKeyPath();

// Made by the service before it starts any application, which can only watch a key that exists.
void CreateUpdateKey();

bool ReadUpdateStatus(UpdateStatus* status);

void WriteUpdateState(UpdateState state, const std::wstring& target, const std::wstring& message = {});
void WriteUpdateProgress(DWORD progress);

uint64_t ReadUpdateNumber(const wchar_t* name);
void WriteUpdateNumber(const wchar_t* name, uint64_t value);
std::wstring ReadUpdateText(const wchar_t* name);
void WriteUpdateText(const wchar_t* name, const std::wstring& value);

}  // namespace sstk
