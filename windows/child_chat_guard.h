#pragma once

#include <string>

// Pyokomin (ADR-0064): where the chat window may navigate to.
//
// Only YouTube's live chat pages. The path is checked up to its end (the
// query), so that "live_chatX" or "live_chat.evil" do not pass.
// Kept as a pure function so that it can be tested on its own.
inline bool IsAllowedChatUrl(const std::wstring& url) {
  static const wchar_t* const kAllowed[] = {
      L"https://www.youtube.com/live_chat?",
      L"https://www.youtube.com/live_chat_replay?",
  };
  for (const auto* prefix : kAllowed) {
    const std::wstring p(prefix);
    if (url.compare(0, p.size(), p) == 0) {
      return true;
    }
  }
  return false;
}
