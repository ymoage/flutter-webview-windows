#pragma once

#include <windows.h>

// Pyokomin (ADR-0065): how the child windows share the app window.
//
// From left to right: the page | the splitter | the chat area. They never
// overlap, and only the native side places them (the page does not send
// positions). Kept as a pure function so that the edges can be tested.

// Logical pixels (96 dpi).
constexpr int kSplitterWidthDip = 6;
constexpr int kChatMinWidthDip = 280;
// The chat area may grow up to (window width - this).
constexpr int kChatMaxGapDip = 320;
// Used until the app sets the width from its settings.
constexpr int kChatDefaultWidthDip = 360;

struct LayoutRect {
  int x;
  int y;
  int width;
  int height;
};

struct ChildLayout {
  LayoutRect page;
  LayoutRect splitter;
  LayoutRect chat;
  // False when the chat area is closed (the page fills the window and the
  // splitter and chat rectangles are empty).
  bool chat_open;
  // The chat width actually used, in logical pixels (after the limits).
  int chat_width_dip;
};

inline int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

inline int PxToDip(int px, UINT dpi) {
  return MulDiv(px, 96, static_cast<int>(dpi));
}

// [client_width] / [client_height] are physical pixels, [chat_width_dip] is
// the wanted width of the chat area in logical pixels.
//  - Closed: the page fills the window.
//  - Open: the chat area is kept between 280 and (window - 320) logical
//    pixels. When the window is too narrow for both, the chat area stays at
//    280 and the page shrinks (never below 0).
inline ChildLayout LayoutChildren(int client_width, int client_height,
                                  int chat_width_dip, UINT dpi,
                                  bool chat_open) {
  ChildLayout layout = {};
  if (client_width < 0) {
    client_width = 0;
  }
  if (client_height < 0) {
    client_height = 0;
  }
  if (!chat_open) {
    layout.page = {0, 0, client_width, client_height};
    layout.chat_open = false;
    layout.chat_width_dip = chat_width_dip;
    return layout;
  }
  const int splitter = DipToPx(kSplitterWidthDip, dpi);
  const int chat_min = DipToPx(kChatMinWidthDip, dpi);
  const int chat_max = client_width - DipToPx(kChatMaxGapDip, dpi);
  int chat = DipToPx(chat_width_dip, dpi);
  if (chat > chat_max) {
    chat = chat_max;
  }
  if (chat < chat_min) {
    chat = chat_min;
  }
  int page = client_width - splitter - chat;
  if (page < 0) {
    page = 0;
  }
  layout.page = {0, 0, page, client_height};
  layout.splitter = {page, 0, splitter, client_height};
  layout.chat = {page + splitter, 0, chat, client_height};
  layout.chat_open = true;
  layout.chat_width_dip = PxToDip(chat, dpi);
  return layout;
}
