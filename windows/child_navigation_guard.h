#pragma once

#include <string>

// Pyokomin (instruction 444/445, design-web-ui 10-2): where the page in the
// child window may go.
//
// A top-level navigation stays inside the page's own origin (the virtual
// host, "https://app.pyokomin.local/"). Anything else is cancelled; http and
// https links are handed to the default browser, other schemes are only
// cancelled. Navigations inside iframes (YouTube) are not decided here.
// Kept as a pure function so that it can be tested on its own.
enum class TopNavigation { kAllow, kOpenExternally, kBlock };

inline bool IsWebUrl(const std::wstring& url) {
  static const wchar_t* const kSchemes[] = {L"https://", L"http://"};
  for (const auto* scheme : kSchemes) {
    const std::wstring s(scheme);
    if (url.compare(0, s.size(), s) == 0) {
      return true;
    }
  }
  return false;
}

// [allowed_origin] ends with "/" so that "https://app.pyokomin.local.evil/"
// does not pass.
inline TopNavigation DecideTopNavigation(const std::wstring& url,
                                         const std::wstring& allowed_origin) {
  if (!allowed_origin.empty() &&
      url.compare(0, allowed_origin.size(), allowed_origin) == 0) {
    return TopNavigation::kAllow;
  }
  return IsWebUrl(url) ? TopNavigation::kOpenExternally
                       : TopNavigation::kBlock;
}
