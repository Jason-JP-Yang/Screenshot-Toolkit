#include "dragdrop.h"

#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace sstk {

namespace {

class DropSource final : public IDropSource {
 public:
  DropSource(HWND controller, uint64_t id, const std::atomic<bool>& cancel) : controller_(controller), id_(id), cancel_(cancel) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (iid == IID_IUnknown || iid == IID_IDropSource) {
      *out = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG remaining = InterlockedDecrement(&references_);
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP QueryContinueDrag(BOOL escape_pressed, DWORD) override {
    // Read from the hardware: this thread never saw the press, and the keyboard belongs to the foreground app.
    const bool escape = escape_pressed || (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (escape || cancel_.load(std::memory_order_relaxed)) {
      cancelled_ = true;
      GetCursorPos(&release_);
      return DRAGDROP_S_CANCEL;
    }
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if ((GetAsyncKeyState(button) & 0x8000) == 0) {
      GetCursorPos(&release_);
      return DRAGDROP_S_DROP;
    }
    return S_OK;
  }

  STDMETHODIMP GiveFeedback(DWORD effect) override {
    const bool accepts = effect != DROPEFFECT_NONE;
    if (accepts != accepts_) {
      accepts_ = accepts;
      PostMessageW(controller_, kMsgDragFeedback, static_cast<WPARAM>(id_), accepts ? 1 : 0);
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return S_OK;
  }

  bool cancelled() const { return cancelled_; }
  POINT release() const { return release_; }

 private:
  LONG references_ = 1;
  HWND controller_;
  uint64_t id_;
  const std::atomic<bool>& cancel_;
  bool accepts_ = false;
  bool cancelled_ = false;
  POINT release_{};
};

}  // namespace

void EnsureOleInitialized() {
  static bool initialized = false;
  if (!initialized) {
    (void)OleInitialize(nullptr);
    initialized = true;
  }
}

FileDragThread::FileDragThread(HWND controller, Store& store)
    : controller_(controller), store_(store), ui_thread_(GetCurrentThreadId()), wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
  thread_ = std::thread([this] { Main(); });
}

FileDragThread::~FileDragThread() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  cancel_.store(true, std::memory_order_relaxed);
  SetEvent(wake_);
  if (thread_.joinable()) thread_.join();
  CloseHandle(wake_);
}

void FileDragThread::Begin(std::shared_ptr<Shot> shot, uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    pending_ = std::move(shot);
    pending_id_ = id;
  }
  cancel_.store(false, std::memory_order_relaxed);
  SetEvent(wake_);
}

void FileDragThread::Main() {
  (void)OleInitialize(nullptr);
  for (;;) {
    MsgWaitForMultipleObjectsEx(1, &wake_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    // A target may keep reading from the data object after the drop, so this apartment stays open.
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    std::shared_ptr<Shot> shot;
    uint64_t id = 0;
    {
      std::lock_guard lock(mutex_);
      if (stopping_) break;
      shot = std::move(pending_);
      pending_ = nullptr;
      id = pending_id_;
    }
    if (shot) Run(shot, id);
  }
  OleUninitialize();
}

void FileDragThread::Run(const std::shared_ptr<Shot>& shot, uint64_t id) {
  auto outcome = std::make_unique<FileDragOutcome>();
  outcome->id = id;
  outcome->cancelled = true;
  GetCursorPos(&outcome->release);

  const std::wstring path = store_.WaitUntilSaved(shot);
  ComPtr<IShellItem> item;
  ComPtr<IDataObject> data;
  if (!path.empty() && !cancel_.load(std::memory_order_relaxed) &&
      SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
      SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
    // The press landed on the UI thread, and the loop expects the capture and button state it left.
    const DWORD self = GetCurrentThreadId();
    const BOOL attached = AttachThreadInput(self, ui_thread_, TRUE);

    auto* source = new DropSource(controller_, id, cancel_);
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    outcome->effect = result == DRAGDROP_S_DROP ? effect : DROPEFFECT_NONE;
    outcome->cancelled = source->cancelled();
    outcome->release = source->release();
    Trace("drag: finished %08lx effect %lu cancelled %d", result, outcome->effect, outcome->cancelled);
    source->Release();

    if (attached) AttachThreadInput(self, ui_thread_, FALSE);
  } else {
    Trace("drag: file not available");
  }

  if (PostMessageW(controller_, kMsgDragFinished, 0, reinterpret_cast<LPARAM>(outcome.get()))) outcome.release();
}

}  // namespace sstk
