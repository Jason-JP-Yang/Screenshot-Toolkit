#include "dragdrop.h"

#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>

namespace sstk {

namespace {

class DropSource final : public IDropSource {
 public:
  STDMETHODIMP QueryInterface(REFIID id, void** out) override {
    if (id == IID_IUnknown || id == IID_IDropSource) {
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
  STDMETHODIMP QueryContinueDrag(BOOL escape_pressed, DWORD key_state) override {
    if (escape_pressed) return DRAGDROP_S_CANCEL;
    if ((key_state & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
    return S_OK;
  }
  STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

 private:
  LONG references_ = 1;
};

void EnsureOleInitialized() {
  static bool initialized = false;
  if (!initialized) {
    (void)OleInitialize(nullptr);
    initialized = true;
  }
}

HBITMAP CreateDragBitmap(const Image& preview) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = preview.width();
  info.bmiHeader.biHeight = -preview.height();
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bitmap != nullptr) memcpy(bits, preview.data(), static_cast<size_t>(preview.stride()) * preview.height());
  return bitmap;
}

}  // namespace

DWORD RunFileDrag(const std::wstring& path, const Image& preview) {
  EnsureOleInitialized();

  ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return DROPEFFECT_NONE;
  ComPtr<IDataObject> data;
  if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return DROPEFFECT_NONE;

  ComPtr<IDragSourceHelper> helper;
  if (preview && SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) {
    SHDRAGIMAGE image{};
    image.sizeDragImage = SIZE{preview.width(), preview.height()};
    image.ptOffset = POINT{preview.width() / 2, preview.height() / 2};
    image.hbmpDragImage = CreateDragBitmap(preview);
    image.crColorKey = CLR_NONE;
    // On success the helper owns the bitmap; on failure it is still ours to free.
    if (image.hbmpDragImage != nullptr && FAILED(helper->InitializeFromBitmap(&image, data.Get()))) {
      DeleteObject(image.hbmpDragImage);
    }
  }

  auto* source = new DropSource();
  DWORD effect = DROPEFFECT_NONE;
  HRESULT hr = DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
  source->Release();
  return hr == DRAGDROP_S_DROP ? effect : DROPEFFECT_NONE;
}

}  // namespace sstk
