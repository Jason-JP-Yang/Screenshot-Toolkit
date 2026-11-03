// Top-level controller of everything visible: one Deck per monitor that has been used.
#pragma once

#include "capture.h"
#include "compositor.h"
#include "store.h"

#include <memory>
#include <vector>

namespace sstk {

class Card;
class Deck;
class DragSession;
class FileDragThread;
struct FileDragOutcome;

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

  void OnDragFrame();
  void OnDragFeedback(uint64_t id, bool accepts);
  void OnDragFinished(std::unique_ptr<FileDragOutcome> outcome);

  // True when nothing is on screen, so memory can be returned.
  bool IsIdle() const;

  static constexpr UINT_PTR kTickTimer = 1;

 private:
  friend class Deck;
  friend class DragSession;

  Deck* DeckFor(HMONITOR monitor);
  void EnsureTicking();
  void BeginDrag(Deck& deck, std::unique_ptr<Card> card, size_t index, POINT press, const TimeBase& now);
  void CancelDrags();
  bool drag_held() const { return session_ != nullptr; }

  HWND controller_;
  Compositor& compositor_;
  Store& store_;
  std::vector<std::unique_ptr<Deck>> decks_;
  bool ticking_ = false;
  bool active_ = false;

  FramePacer pacer_;
  std::unique_ptr<FileDragThread> file_drag_;
  std::unique_ptr<DragSession> session_;                // The thumbnail held by the pointer, if any.
  std::vector<std::unique_ptr<DragSession>> finishing_;  // Released, and still playing out what came of it.
  uint64_t next_drag_id_ = 1;
  double last_frame_ = 0.0;
};

}  // namespace sstk
