// Top-level controller of everything visible: one Deck per monitor that has been used.
#pragma once

#include "capture.h"
#include "compositor.h"
#include "store.h"

#include <memory>
#include <vector>

namespace sstk {

class Deck;

class Overlay {
 public:
  Overlay(HWND controller, Compositor& compositor, Store& store);
  ~Overlay();

  // Creates the deck for the current monitor up front so that the very first press has nothing to set up.
  void Warmup();

  void OnFlash(HMONITOR monitor);
  void OnCaptured(std::unique_ptr<CaptureResult> capture);
  void OnTick();
  void OnDisplayChange();

  // True when nothing is on screen, so memory can be returned.
  bool IsIdle() const;

  static constexpr UINT_PTR kTickTimer = 1;

 private:
  Deck* DeckFor(HMONITOR monitor);
  void EnsureTicking();
  bool AnyDeckInModalDrag() const;

  HWND controller_;
  Compositor& compositor_;
  Store& store_;
  std::vector<std::unique_ptr<Deck>> decks_;
  bool ticking_ = false;
  bool active_ = false;
  bool display_change_pending_ = false;
};

}  // namespace sstk
