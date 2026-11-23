// Top-level controller of everything visible: one Deck per monitor that has been used.
#pragma once

#include "sstk/capture/capture.h"
#include "sstk/capture/hook.h"
#include "sstk/editor/editor.h"
#include "sstk/editor/jobs.h"
#include "sstk/graphics/compositor.h"
#include "sstk/scroll/guard.h"
#include "sstk/scroll/recorder.h"
#include "sstk/scroll/scrollable.h"
#include "sstk/storage/store.h"

#include <memory>
#include <vector>

namespace sstk {

class Card;
class Deck;
class DragSession;
class FileDragThread;
struct FileDragOutcome;

class Overlay : public EditorHost {
 public:
  Overlay(HWND controller, Compositor& compositor, Store& store);
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // Creates the deck for the current monitor up front so that the very first press has nothing to set up.
  void Warmup();

  void OnFlash(HMONITOR monitor);
  void OnCaptured(std::unique_ptr<CaptureResult> capture);
  void OnTick();
  void OnDisplayChange();

  void OnDragFrame();
  void OnDragFeedback(uint64_t id, bool accepts);
  void OnDragFinished(std::unique_ptr<FileDragOutcome> outcome);
  void OnDragCancel();
  void OnSelectorStep(int direction);

  void OnWindowShots(std::unique_ptr<WindowShots> shots);

  void OnScrollAreas(std::unique_ptr<ScrollAreas> found);
  void OnScrollChord(bool from_capture);
  void OnScrollWheel(POINT point);
  void OnScrollUpdate(std::unique_ptr<ScrollUpdate> update);

  // Input that reached the desktop: whatever was shot before it may no longer be as the shot saw it, and is checked on.
  void OnPointerInput(double when, POINT point);
  void OnKeyInput(double when);
  void OnScrollMismatch(uint64_t shot);

  // The editing panel's keys and wheel turns, held for it by the hook, and the work done for it on its own thread.
  void OnEditorKey(UINT key, LPARAM flags);
  void OnEditorWheel(POINT point, int delta, LPARAM flags);
  void OnEditorJob(std::unique_ptr<EditorJobResult> result);

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
  void EndDrag(const FileDragOutcome& outcome);
  void SyncHookMode();
  bool drag_held() const { return session_ != nullptr; }

  void StartScroll(Deck& deck);
  void FinishScroll();
  void CancelScroll();
  void Disturb(double when);
  void GuardScroll(const Card& card, const RECT& monitor);
  Card* HeldCard(const std::shared_ptr<Shot>& shot) const;  // The card of the shot, if it is in the air.

  // The editing panel, opened from a thumbnail or straight from a capture.
  bool OpenEditor(Deck& deck, Card& card);
  void OpenEditorFromCapture(std::unique_ptr<CaptureResult> capture);
  bool EditorOver(HMONITOR monitor) const;
  HWND StageAfter() const;  // Where a deck's windows go in the stacking order: below the panel while it is open.
  Card* EditorCard(Deck** deck) const;
  void CloseEditor();
  EditorLanding EditorLand(Editor& editor, const EditorOutcome& outcome) override;
  void EditorCovering(Editor& editor) override;
  void EditorArrived(Editor& editor, const EditorOutcome& outcome) override;
  void EditorDeleted(Editor& editor) override;

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
  HookMode hook_mode_ = HookMode::kIdle;
  bool watching_input_ = false;

  uint64_t next_shot_id_ = 1;
  std::unique_ptr<ScrollDetector> detector_;
  std::unique_ptr<OfferGuard> guard_;
  std::unique_ptr<ScrollRecorder> recorder_;  // The scrolling screenshot in progress, if any.
  std::vector<std::unique_ptr<ScrollRecorder>> retired_;  // Finished, their threads winding down.
  Deck* scroll_deck_ = nullptr;
  uint64_t scroll_session_ = 0;

  std::unique_ptr<EditorJobs> jobs_;
  std::unique_ptr<Editor> editor_;
  std::shared_ptr<Shot> editor_shot_;  // Whose card, in whichever deck, the panel has or will make.
  bool scroll_after_editor_ = false;
};

}  // namespace sstk
