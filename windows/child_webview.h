#pragma once

#include <WebView2.h>
#include <wil/com.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Pyokomin (ADR-0063, ADR-0064, #298): a webview hosted in a real child
// window of the app window, with a plain ICoreWebView2Controller.
//
// The texture-based webview of this package lives under a message-only
// window and gets the mouse forwarded by hand. On a mouse press the Flutter
// window takes the focus first, the page blurs and YouTube closes its menus
// before the press arrives. A real child window gets the input directly from
// the OS, so this does not happen.
//
// Two roles (ADR-0064, ADR-0065):
//  - kPage gets a virtual host mapped to a local folder.
//  - kChat opens YouTube's live_chat directly. It may only navigate inside
//    live_chat, opens no new windows and takes no drops.
// The owner places them side by side (Place); they never overlap.
//
// All of them share the environment (and so the profile and the sign-in)
// with the other webviews.
// Always owned by a std::shared_ptr: the creation completion handler keeps
// the object alive, so closing while the creation is in flight is safe.
class ChildWebview : public std::enable_shared_from_this<ChildWebview> {
 public:
  enum class Role { kPage, kChat };

  // Reports to the owner. Called on the UI thread; never after Close().
  struct Listener {
    // The webview got the keyboard focus.
    std::function<void(int64_t id)> on_focus;
    // The page hid itself (postMessage "hide").
    std::function<void(int64_t id)> on_hidden_by_page;
    // A web message from an accepted origin, with the source URL.
    std::function<void(int64_t id, const std::string& source,
                       const std::string& message)>
        on_message;
    // A navigation ("navigation") or a new window ("newWindow") was
    // cancelled. The URL is not passed on.
    std::function<void(int64_t id, const std::string& what)> on_blocked;
  };

  typedef std::function<void(bool ok, std::string error)> CreatedCallback;

  ChildWebview(int64_t id, Role role, HWND top_level, HWND flutter_view,
               Listener listener);
  ~ChildWebview();

  ChildWebview(const ChildWebview&) = delete;
  ChildWebview& operator=(const ChildWebview&) = delete;

  // Creates the controller and opens [url]. For kPage, maps [host_name] to
  // [folder] first (both are ignored for kChat).
  // [callback] is called once, from the completion handler.
  void Create(ICoreWebView2Environment* environment,
              const std::string& host_name, const std::string& folder,
              const std::string& url, CreatedCallback callback);

  // Shows (and, for kPage, focuses) or hides the child window. Where the
  // focus goes after hiding is decided by the owner.
  void SetVisible(bool visible);
  bool visible() const { return visible_; }

  // Position and size in physical pixels, in the client coordinates of the
  // app window.
  void Place(const RECT& bounds);

  // Moves the keyboard focus into the webview.
  void Focus();

  // WM_MOVE of the top-level window.
  void OnParentMoved();

  int64_t id() const { return id_; }
  Role role() const { return role_; }

  // Safe to call more than once and after a failed creation.
  void Close();

 private:
  const int64_t id_;
  const Role role_;
  HWND top_level_;
  HWND flutter_view_;
  Listener listener_;
  HWND container_ = nullptr;
  wil::com_ptr<ICoreWebView2Controller> controller_;
  wil::com_ptr<ICoreWebView2> webview_;
  EventRegistrationToken message_token_ = {};
  EventRegistrationToken focus_token_ = {};
  EventRegistrationToken navigation_token_ = {};
  EventRegistrationToken new_window_token_ = {};
  bool visible_ = false;
  bool closed_ = false;
  // Only messages from this origin are accepted (kPage: "https://<host>/",
  // kChat: "https://www.youtube.com/").
  std::wstring allowed_origin_;

  HRESULT OnControllerCreated(HRESULT result,
                              ICoreWebView2Controller* controller,
                              const std::wstring& whost,
                              const std::wstring& wfolder,
                              const std::wstring& wurl,
                              CreatedCallback callback);
  void ApplyControllerBounds();
  void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
  void OnNavigationStarting(ICoreWebView2NavigationStartingEventArgs* args);
};
