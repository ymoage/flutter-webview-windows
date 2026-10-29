#pragma once

#include <windows.h>

// Pyokomin (ADR-0063): who should get the keyboard focus when the top-level
// window becomes active.
//
// The runner gives the focus to the Flutter view on every WM_ACTIVATE. While
// the child-window webview is shown, the focus has to go to WebView2 instead,
// otherwise every activation takes it away from the page (#298).
// Kept as a pure function so that the branch can be tested on its own.
enum class ActivateFocusTarget {
  // Leave the message to the runner (focus goes to the Flutter view).
  kRunner,
  // Give the focus to the child-window webview.
  kChildWebview,
};

inline ActivateFocusTarget DecideActivateFocus(UINT message, WPARAM wparam,
                                               bool child_visible) {
  if (message != WM_ACTIVATE) {
    return ActivateFocusTarget::kRunner;
  }
  if (LOWORD(wparam) == WA_INACTIVE) {
    return ActivateFocusTarget::kRunner;
  }
  return child_visible ? ActivateFocusTarget::kChildWebview
                       : ActivateFocusTarget::kRunner;
}
