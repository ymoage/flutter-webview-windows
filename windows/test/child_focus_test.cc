// Pyokomin (ADR-0063): tests for DecideActivateFocus.
// Run with tool/run_native_tests.ps1 (needs Visual Studio C++ tools).
#include <cstdio>

#include "../child_focus.h"

static int failures = 0;

static void Expect(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

int main() {
  const auto kChild = ActivateFocusTarget::kChildWebview;
  const auto kRunner = ActivateFocusTarget::kRunner;

  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, true) == kChild,
         "activated while shown -> child");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_CLICKACTIVE, true) == kChild,
         "click-activated while shown -> child");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, false) == kRunner,
         "activated while hidden -> runner");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_INACTIVE, true) == kRunner,
         "deactivated while shown -> runner");
  // The high word carries the minimized flag; only the low word counts.
  Expect(DecideActivateFocus(WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 1), true) ==
             kChild,
         "activated (minimized flag) while shown -> child");
  Expect(DecideActivateFocus(WM_SETFOCUS, 0, true) == kRunner,
         "other message -> runner");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
