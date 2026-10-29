#pragma once

#include <WebView2.h>
#include <wil/com.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

// Pyokomin (ADR-0063, #298): a webview hosted in a real child window of the
// app window, with a plain ICoreWebView2Controller.
//
// The texture-based webview of this package lives under a message-only
// window and gets the mouse forwarded by hand. On a mouse press the Flutter
// window takes the focus first, the page blurs and YouTube closes its menus
// before the press arrives. A real child window gets the input directly from
// the OS, so this does not happen.
//
// The child window fills the client area of the app window. It shares the
// environment (and so the profile and the sign-in) with the other webviews.
// Always owned by a std::shared_ptr: the creation completion handler keeps
// the object alive, so closing while the creation is in flight is safe.
class ChildWebview : public std::enable_shared_from_this<ChildWebview> {
 public:
  typedef std::function<void(bool ok, std::string error)> CreatedCallback;

  ChildWebview(HWND top_level, HWND flutter_view);
  ~ChildWebview();

  ChildWebview(const ChildWebview&) = delete;
  ChildWebview& operator=(const ChildWebview&) = delete;

  // Creates the controller, maps [host_name] to [folder] and opens [url].
  // [callback] is called once, from the completion handler.
  void Create(ICoreWebView2Environment* environment,
              const std::string& host_name, const std::string& folder,
              const std::string& url, CreatedCallback callback);

  // Shows (and focuses) or hides the child window. Hiding gives the focus
  // back to the Flutter view.
  void SetVisible(bool visible);
  bool visible() const { return visible_; }

  // Messages of the top-level window (registered as a window proc delegate).
  // Returns a value only for the messages it takes over (WM_ACTIVATE while
  // shown); everything else is left to the runner.
  std::optional<LRESULT> HandleTopLevelMessage(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam);

  // Safe to call more than once and after a failed creation.
  void Close();

 private:
  HWND top_level_;
  HWND flutter_view_;
  HWND container_ = nullptr;
  wil::com_ptr<ICoreWebView2Controller> controller_;
  wil::com_ptr<ICoreWebView2> webview_;
  EventRegistrationToken message_token_ = {};
  bool visible_ = false;
  bool closed_ = false;
  // Only messages from this origin are accepted ("https://<host>/").
  std::wstring allowed_origin_;

  HRESULT OnControllerCreated(HRESULT result,
                              ICoreWebView2Controller* controller,
                              const std::wstring& whost,
                              const std::wstring& wfolder,
                              const std::wstring& wurl,
                              CreatedCallback callback);
  void FitToParent();
  void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
};
