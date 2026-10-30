#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

// Pyokomin (ADR-0063, ADR-0064): who should get the keyboard focus when the
// top-level window becomes active.
//
// The runner gives the focus to the Flutter view on every WM_ACTIVATE. While
// a child-window webview is shown, the focus has to go to WebView2 instead,
// otherwise every activation takes it away from the page (#298).
// With several child windows (the page and the chat), it goes back to the
// one that had it last.
// Kept as a pure function so that the branch can be tested on its own.

// Returned when the message is left to the runner (focus goes to the
// Flutter view).
constexpr int64_t kFocusRunner = -1;

struct ChildFocusCandidate {
  int64_t id;
  bool visible;
  // The page fills the window; the others (the chat) sit on top of it.
  bool is_page;
};

// [last_focused] is the id of the child that got the focus last
// (kFocusRunner when none has). Returns the id of the child to focus, or
// kFocusRunner.
//  - The child that had the focus last, if it is shown.
//  - Otherwise the shown page (the chat is hidden or never had the focus).
//  - Otherwise the runner.
inline int64_t DecideActivateFocus(
    UINT message, WPARAM wparam, int64_t last_focused,
    const std::vector<ChildFocusCandidate>& children) {
  if (message != WM_ACTIVATE) {
    return kFocusRunner;
  }
  if (LOWORD(wparam) == WA_INACTIVE) {
    return kFocusRunner;
  }
  for (const auto& child : children) {
    if (child.id == last_focused && child.visible) {
      return child.id;
    }
  }
  for (const auto& child : children) {
    if (child.is_page && child.visible) {
      return child.id;
    }
  }
  return kFocusRunner;
}
