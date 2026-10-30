// Pyokomin (ADR-0063, ADR-0064): tests for DecideActivateFocus.
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
  constexpr int64_t kPage = 1;
  constexpr int64_t kChat = 2;
  const std::vector<ChildFocusCandidate> page_only_shown = {
      {kPage, true, true}};
  const std::vector<ChildFocusCandidate> page_only_hidden = {
      {kPage, false, true}};
  const std::vector<ChildFocusCandidate> both_shown = {{kPage, true, true},
                                                       {kChat, true, false}};
  const std::vector<ChildFocusCandidate> chat_hidden = {{kPage, true, true},
                                                        {kChat, false, false}};
  const std::vector<ChildFocusCandidate> both_hidden = {{kPage, false, true},
                                                        {kChat, false, false}};

  // One child (ADR-0063).
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kPage, page_only_shown) ==
             kPage,
         "activated while shown -> child");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_CLICKACTIVE, kPage,
                             page_only_shown) == kPage,
         "click-activated while shown -> child");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kPage,
                             page_only_hidden) == kFocusRunner,
         "activated while hidden -> runner");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_INACTIVE, kPage,
                             page_only_shown) == kFocusRunner,
         "deactivated while shown -> runner");
  // The high word carries the minimized flag; only the low word counts.
  Expect(DecideActivateFocus(WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 1), kPage,
                             page_only_shown) == kPage,
         "activated (minimized flag) while shown -> child");
  Expect(DecideActivateFocus(WM_SETFOCUS, 0, kPage, page_only_shown) ==
             kFocusRunner,
         "other message -> runner");

  // Page and chat (ADR-0064).
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kChat, both_shown) ==
             kChat,
         "chat had the focus last and is shown -> chat");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kPage, both_shown) ==
             kPage,
         "page had the focus last -> page (not the chat on top)");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kChat, chat_hidden) ==
             kPage,
         "chat had the focus but is hidden -> shown page");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kFocusRunner,
                             both_shown) == kPage,
         "no child had the focus yet -> shown page");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, kChat, both_hidden) ==
             kFocusRunner,
         "nothing shown -> runner");
  Expect(DecideActivateFocus(WM_ACTIVATE, WA_ACTIVE, 99, both_shown) == kPage,
         "unknown last id -> shown page");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
