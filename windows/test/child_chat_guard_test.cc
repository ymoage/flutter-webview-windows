// Pyokomin (ADR-0064): tests for IsAllowedChatUrl.
// Run with tool/run_native_tests.ps1 (needs Visual Studio C++ tools).
#include <cstdio>

#include "../child_chat_guard.h"

static int failures = 0;

static void Expect(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

int main() {
  Expect(IsAllowedChatUrl(
             L"https://www.youtube.com/live_chat?v=abcdefghijk&is_popout=1"),
         "live_chat with a query -> allowed");
  Expect(IsAllowedChatUrl(L"https://www.youtube.com/live_chat_replay?"
                          L"continuation=x"),
         "live_chat_replay with a query -> allowed");
  Expect(!IsAllowedChatUrl(L"https://www.youtube.com/live_chatX?v=1"),
         "live_chatX -> refused (path checked to its end)");
  Expect(!IsAllowedChatUrl(L"https://www.youtube.com/live_chat.evil/?v=1"),
         "live_chat.evil -> refused");
  Expect(!IsAllowedChatUrl(L"https://www.youtube.com/watch?v=abcdefghijk"),
         "watch page -> refused");
  Expect(!IsAllowedChatUrl(L"https://www.youtube.com.evil/live_chat?v=1"),
         "other host -> refused");
  Expect(!IsAllowedChatUrl(L"http://www.youtube.com/live_chat?v=1"),
         "http -> refused");
  Expect(!IsAllowedChatUrl(L""), "empty -> refused");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
