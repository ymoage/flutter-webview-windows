// Pyokomin (instruction 445): tests for DecideTopNavigation.
// Run with tool/run_native_tests.ps1 (needs Visual Studio C++ tools).
#include <cstdio>

#include "../child_navigation_guard.h"

static int failures = 0;

static void Expect(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

int main() {
  const std::wstring host = L"https://app.pyokomin.local/";
  Expect(DecideTopNavigation(L"https://app.pyokomin.local/index.html", host) ==
             TopNavigation::kAllow,
         "own page -> allowed");
  Expect(DecideTopNavigation(L"https://app.pyokomin.local/a/b?x=1#y", host) ==
             TopNavigation::kAllow,
         "own page with a path and a query -> allowed");
  Expect(DecideTopNavigation(L"https://app.pyokomin.local.evil/", host) ==
             TopNavigation::kOpenExternally,
         "look-alike host -> not allowed (to the browser)");
  Expect(DecideTopNavigation(L"https://www.youtube.com/watch?v=1", host) ==
             TopNavigation::kOpenExternally,
         "https link -> to the browser");
  Expect(DecideTopNavigation(L"http://example.com/", host) ==
             TopNavigation::kOpenExternally,
         "http link -> to the browser");
  Expect(DecideTopNavigation(L"file:///C:/Windows/", host) ==
             TopNavigation::kBlock,
         "file -> cancelled only");
  Expect(DecideTopNavigation(L"javascript:alert(1)", host) ==
             TopNavigation::kBlock,
         "javascript -> cancelled only");
  Expect(DecideTopNavigation(L"about:blank", host) == TopNavigation::kBlock,
         "about:blank -> cancelled only");
  Expect(DecideTopNavigation(L"https://app.pyokomin.local/", L"") ==
             TopNavigation::kOpenExternally,
         "no origin yet -> nothing is the own page");

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
