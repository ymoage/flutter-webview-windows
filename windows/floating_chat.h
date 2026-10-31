#pragma once

#include <WebView2.h>
#include <wil/com.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Pyokomin (ADR-0067, #298): YouTube's real chat in a floating window.
//
// The chat area of the app embeds live_chat in an iframe with embed_domain,
// where YouTube leaves out the moderation items (delete, ...). This window
// opens live_chat directly, so the user can delete there. It is owned by the
// app window (instruction 454): it stays in front of the app, is hidden with
// it when minimized and has no taskbar button of its own. It is neither
// modal (the app stays usable) nor topmost. Where it goes, and whether it
// stays open, is up to the user. The app only opens it, brings it
// to the front, guards it and closes it when the app ends.
//
// Guards: it may only navigate inside live_chat (IsAllowedChatUrl), opens no
// new windows, takes no drops, and only messages from
// https://www.youtube.com/ are passed on.
// Always owned by a std::shared_ptr: the creation completion handler keeps
// the object alive, so closing while the creation is in flight is safe.
class FloatingChat : public std::enable_shared_from_this<FloatingChat> {
 public:
  // Reports to the owner. Called on the UI thread; never after Close().
  struct Listener {
    // The user closed the window (the close button). The owner disposes it.
    std::function<void(int64_t id)> on_closed;
    // A navigation ("navigation") or a new window ("newWindow") was
    // cancelled. Only the origin of the target is passed on (OriginOf: up
    // to the first '/' after "://"; instruction 444), never the path or the
    // query.
    std::function<void(int64_t id, const std::string& what,
                       const std::string& origin)>
        on_blocked;
  };

  typedef std::function<void(bool ok, std::string error)> CreatedCallback;

  FloatingChat(int64_t id, Listener listener);
  ~FloatingChat();

  FloatingChat(const FloatingChat&) = delete;
  FloatingChat& operator=(const FloatingChat&) = delete;

  // Creates the window and the controller and opens [url] (checked by the
  // owner with IsAllowedChatUrl). [callback] is called once.
  // [owner] is the app's top-level window; without it the creation fails.
  void Create(ICoreWebView2Environment* environment, HWND owner,
              const std::string& url, CreatedCallback callback);

  // Restores it if minimized and brings it to the front.
  void BringToFront();

  // Safe to call more than once and after a failed creation.
  void Close();

 private:
  const int64_t id_;
  Listener listener_;
  HWND window_ = nullptr;
  wil::com_ptr<ICoreWebView2Controller> controller_;
  wil::com_ptr<ICoreWebView2> webview_;
  EventRegistrationToken navigation_token_ = {};
  EventRegistrationToken new_window_token_ = {};
  bool closed_ = false;

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  HRESULT OnControllerCreated(HRESULT result,
                              ICoreWebView2Controller* controller,
                              const std::wstring& wurl,
                              CreatedCallback callback);
  void FitController();
  void FocusWebview();
  void OnNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args);
};
