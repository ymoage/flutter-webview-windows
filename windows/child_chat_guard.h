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

// Pyokomin (instruction 444): the origin of [url] for the records, that is,
// everything up to the first '/' after "://" (no path, no query, no
// fragment). Empty when [url] has no "://".
inline std::wstring OriginOf(const std::wstring& url) {
  const auto scheme_end = url.find(L"://");
  if (scheme_end == std::wstring::npos) {
    return L"";
  }
  const auto host_start = scheme_end + 3;
  const auto host_end = url.find_first_of(L"/?#", host_start);
  return url.substr(0, host_end == std::wstring::npos ? url.size() : host_end);
}
