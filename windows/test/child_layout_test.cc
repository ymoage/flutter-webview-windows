// Pyokomin (ADR-0065): tests for LayoutChildren.
// Run with tool/run_native_tests.ps1 (needs Visual Studio C++ tools).
#include <cstdio>

#include "../child_layout.h"

static int failures = 0;

static void Expect(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    failures++;
  }
}

static bool Is(const LayoutRect& r, int x, int y, int width, int height) {
  return r.x == x && r.y == y && r.width == width && r.height == height;
}

int main() {
  // 96 dpi, window 1000 x 700: the chat area may be 280 .. 680.
  {
    const auto l = LayoutChildren(1000, 700, 280, 96, true);
    Expect(l.chat_open && Is(l.page, 0, 0, 714, 700) &&
               Is(l.splitter, 714, 0, 6, 700) && Is(l.chat, 720, 0, 280, 700),
           "chat exactly 280 -> page | splitter | chat side by side");
  }
  {
    const auto l = LayoutChildren(1000, 700, 680, 96, true);
    Expect(Is(l.chat, 320, 0, 680, 700) && Is(l.page, 0, 0, 314, 700),
           "chat exactly window - 320 -> kept");
  }
  {
    const auto l = LayoutChildren(1000, 700, 700, 96, true);
    Expect(l.chat.width == 680 && l.chat_width_dip == 680,
           "chat wider than window - 320 -> 680");
  }
  {
    const auto l = LayoutChildren(1000, 700, 200, 96, true);
    Expect(l.chat.width == 280 && l.chat_width_dip == 280,
           "chat narrower than 280 -> 280");
  }
  // Too narrow for both: the chat stays at 280 and the page shrinks.
  {
    const auto l = LayoutChildren(500, 700, 360, 96, true);
    Expect(Is(l.page, 0, 0, 214, 700) && Is(l.chat, 220, 0, 280, 700),
           "narrow window -> chat 280, page shrinks");
  }
  {
    const auto l = LayoutChildren(200, 700, 360, 96, true);
    Expect(l.page.width == 0 && Is(l.splitter, 0, 0, 6, 700) &&
               Is(l.chat, 6, 0, 280, 700),
           "very narrow window -> page 0 (not negative)");
  }
  {
    const auto l = LayoutChildren(1000, 700, 360, 96, false);
    Expect(!l.chat_open && Is(l.page, 0, 0, 1000, 700) &&
               l.splitter.width == 0 && l.chat.width == 0,
           "closed -> page fills the window");
  }
  // 150 percent: logical pixels are scaled.
  {
    const auto l = LayoutChildren(1500, 900, 360, 144, true);
    Expect(Is(l.splitter, 951, 0, 9, 900) && Is(l.chat, 960, 0, 540, 900) &&
               l.chat_width_dip == 360,
           "dpi 144 -> splitter 9px, chat 540px, width kept in dip");
  }
  {
    // window - 320 dip = 1500 - 480 = 1020 px = 680 dip.
    const auto l = LayoutChildren(1500, 900, 900, 144, true);
    Expect(l.chat.width == 1020 && l.chat_width_dip == 680,
           "dpi 144 -> the upper limit is scaled too");
  }

  std::printf("%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
