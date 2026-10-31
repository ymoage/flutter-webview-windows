#pragma once

#include <WebView2.h>
#include <wil/com.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Pyokomin (ADR-0063, ADR-0067, #298): the app's page in a real child window
// of the app window, with a plain ICoreWebView2Controller.
//
// The texture-based webview of this package lives under a message-only
// window and gets the mouse forwarded by hand. On a mouse press the Flutter
// window takes the focus first, the page blurs and YouTube closes its menus
// before the press arrives. A real child window gets the input directly from
// the OS, so this does not happen.
//
// It fills the client area of the app window and gets a virtual host mapped
// to a local folder. It shares the environment (and so the profile and the
// sign-in) with the other webviews. (The chat that ADR-0064 placed in a
// second child window lives in FloatingChat since ADR-0067.)
// Always owned by a std::shared_ptr: the creation completion handler keeps
// the object alive, so closing while the creation is in flight is safe.
class ChildWebview : public std::enable_shared_from_this<ChildWebview> {
 public:
  // Reports to the owner. Called on the UI thread; never after Close().
  struct Listener {
    // The page hid itself (postMessage "hide").
    std::function<void(int64_t id)> on_hidden_by_page;
    // A web message from the page's own origin, with the source URL.
    std::function<void(int64_t id, const std::string& source,
                       const std::string& message)>
        on_message;
    // A web message from a YouTube live chat iframe of the page
    // (instruction 444), with the frame's source URL.
    std::function<void(int64_t id, const std::string& source,
                       const std::string& message)>
        on_frame_message;
    // A top-level navigation or a new window of the page went outside its
    // own origin and was cancelled (instruction 445). [external] tells
    // whether it was handed to the default browser (http and https only).
    // Only the origin of the target is passed on (OriginOf).
    std::function<void(int64_t id, const std::string& what,
                       const std::string& origin, bool external)>
        on_navigation_blocked;
  };

  typedef std::function<void(bool ok, std::string error)> CreatedCallback;

  ChildWebview(int64_t id, HWND top_level, HWND flutter_view,
               Listener listener);
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

  // Moves the keyboard focus into the webview (only while shown).
  void Focus();

  // Sends a string to the page (window.chrome.webview "message" event).
  // Dropped while the webview is not created yet.
  void PostMessage(const std::string& message);

  // Sends JSON to the page (the "message" event gets the parsed value;
  // design-web-ui 6). Dropped while the webview is not created yet.
  void PostJson(const std::string& json);

  // Pyokomin (instruction 444): a script run in every YouTube live chat
  // iframe of the page (IsAllowedChatUrl) when its document has loaded, and
  // at once in those already loaded. Messages the iframe posts with
  // window.chrome.webview.postMessage come back through on_frame_message.
  // An empty script stops running it in new documents.
  void SetFrameScript(const std::string& script);

  // Pyokomin (instruction 454 B, a developer trial): runs the fixed
  // expression "0" in this page only, marked as started by the user
  // (DevTools Runtime.evaluate userGesture), so that the page counts as
  // activated for autoplay. Nothing on the page is clicked. [done] gets
  // whether the call succeeded.
  void GrantUserGesture(std::function<void(bool ok)> done);

  // WM_SIZE / WM_MOVE of the top-level window.
  void OnParentSized();
  void OnParentMoved();

  int64_t id() const { return id_; }

  // Safe to call more than once and after a failed creation.
  void Close();

 private:
  const int64_t id_;
  HWND top_level_;
  HWND flutter_view_;
  Listener listener_;
  HWND container_ = nullptr;
  wil::com_ptr<ICoreWebView2Controller> controller_;
  wil::com_ptr<ICoreWebView2> webview_;
  EventRegistrationToken message_token_ = {};
  EventRegistrationToken navigation_token_ = {};
  EventRegistrationToken new_window_token_ = {};
  bool visible_ = false;
  bool closed_ = false;
  // Only messages from this origin are accepted ("https://<host>/").
  std::wstring allowed_origin_;

  // The iframes of the page (instruction 444).
  struct Frame {
    wil::com_ptr<ICoreWebView2Frame2> frame;
    std::wstring url;
    bool loaded = false;
    EventRegistrationToken navigation_token = {};
    EventRegistrationToken loaded_token = {};
    EventRegistrationToken message_token = {};
    EventRegistrationToken destroyed_token = {};
  };
  std::vector<std::shared_ptr<Frame>> frames_;
  std::wstring frame_script_;
  EventRegistrationToken frame_created_token_ = {};

  HRESULT OnControllerCreated(HRESULT result,
                              ICoreWebView2Controller* controller,
                              const std::wstring& whost,
                              const std::wstring& wfolder,
                              const std::wstring& wurl,
                              CreatedCallback callback);
  void FitToParent();
  void OnWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
  // Cancels a top-level navigation ([navigation] set) or a new window outside
  // the page's origin; [external] hands the target to the default browser
  // (decided by DecideTopNavigation: web links clicked by the user only).
  void LeavePage(const std::wstring& target, const char* what, bool external,
                 ICoreWebView2NavigationStartingEventArgs* navigation);
  void OnFrameCreated(ICoreWebView2Frame* frame);
  void RunFrameScript(const Frame& frame);
  void RemoveFrame(const Frame* frame);
  static void DetachFrame(Frame& frame);
};
