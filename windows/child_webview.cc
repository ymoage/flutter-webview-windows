#include "child_webview.h"

#include <shellapi.h>
#include <wrl.h>

#include <algorithm>

#include "child_chat_guard.h"
#include "child_navigation_guard.h"
#include "util/string_converter.h"

using namespace Microsoft::WRL;

namespace {

constexpr wchar_t kContainerClassName[] = L"PyokominChildWebviewHost";
// The page asks to be hidden with window.chrome.webview.postMessage("hide").
constexpr wchar_t kHideMessage[] = L"hide";

void EnsureContainerClass() {
  static bool registered = false;
  if (registered) {
    return;
  }
  WNDCLASS window_class = {};
  window_class.lpszClassName = kContainerClassName;
  window_class.lpfnWndProc = &DefWindowProc;
  window_class.hInstance = GetModuleHandle(nullptr);
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClass(&window_class);
  registered = true;
}

}  // namespace

ChildWebview::ChildWebview(int64_t id, HWND top_level, HWND flutter_view,
                           Listener listener)
    : id_(id),
      top_level_(top_level),
      flutter_view_(flutter_view),
      listener_(std::move(listener)) {}

ChildWebview::~ChildWebview() { Close(); }

void ChildWebview::Create(ICoreWebView2Environment* environment,
                          const std::string& host_name,
                          const std::string& folder, const std::string& url,
                          CreatedCallback callback) {
  EnsureContainerClass();
  RECT client = {};
  GetClientRect(top_level_, &client);
  container_ = CreateWindowEx(
      0, kContainerClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
      0, 0, client.right - client.left, client.bottom - client.top, top_level_,
      nullptr, GetModuleHandle(nullptr), nullptr);
  if (!container_) {
    callback(false, "Creating the child window failed.");
    return;
  }
  // The Flutter view is a sibling. Without this it may paint over the child.
  // Not undone on close: in the Web UI the child window is always there
  // (ADR-0063), and the style only clips the view against its siblings, so it
  // changes nothing once they are gone.
  LONG_PTR view_style = GetWindowLongPtr(flutter_view_, GWL_STYLE);
  SetWindowLongPtr(flutter_view_, GWL_STYLE, view_style | WS_CLIPSIBLINGS);

  allowed_origin_ = L"https://" + util::Utf16FromUtf8(host_name) + L"/";
  const std::wstring whost = util::Utf16FromUtf8(host_name);
  const std::wstring wfolder = util::Utf16FromUtf8(folder);
  const std::wstring wurl = util::Utf16FromUtf8(url);

  auto hr = environment->CreateCoreWebView2Controller(
      container_,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [self = shared_from_this(), whost, wfolder, wurl, callback](
              HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            return self->OnControllerCreated(result, controller, whost,
                                             wfolder, wurl, callback);
          })
          .Get());
  if (FAILED(hr)) {
    callback(false, "CreateCoreWebView2Controller could not be started.");
  }
}

HRESULT ChildWebview::OnControllerCreated(HRESULT result,
                                          ICoreWebView2Controller* controller,
                                          const std::wstring& whost,
                                          const std::wstring& wfolder,
                                          const std::wstring& wurl,
                                          CreatedCallback callback) {
  if (closed_) {
    // Closed while the creation was in flight.
    if (SUCCEEDED(result) && controller) {
      controller->Close();
    }
    callback(false, "Closed before the creation finished.");
    return S_OK;
  }
  if (FAILED(result) || !controller) {
    callback(false, "CreateCoreWebView2Controller failed.");
    return S_OK;
  }
  controller_ = controller;
  controller_->get_CoreWebView2(webview_.put());
  if (!webview_) {
    callback(false, "The controller has no CoreWebView2.");
    return S_OK;
  }

  auto webview3 = webview_.try_query<ICoreWebView2_3>();
  if (!webview3 ||
      FAILED(webview3->SetVirtualHostNameToFolderMapping(
          whost.c_str(), wfolder.c_str(),
          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
    callback(false, "Mapping the virtual host failed.");
    return S_OK;
  }

  webview_->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2WebMessageReceivedEventArgs* args)
              -> HRESULT {
            OnWebMessage(args);
            return S_OK;
          })
          .Get(),
      &message_token_);

  // The page stays on its own origin (instruction 445). Iframe navigations
  // are not top-level and do not come here.
  webview_->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string uri;
            const std::wstring target = SUCCEEDED(args->get_Uri(&uri)) && uri
                                            ? std::wstring(uri.get())
                                            : std::wstring();
            if (DecideTopNavigation(target, allowed_origin_) !=
                TopNavigation::kAllow) {
              LeavePage(target, "navigation", true, args);
            }
            return S_OK;
          })
          .Get(),
      &navigation_token_);
  webview_->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [this](ICoreWebView2* sender,
                 ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            // Handled without a new window: nothing opens in the app.
            args->put_Handled(TRUE);
            wil::unique_cotaskmem_string uri;
            const std::wstring target = SUCCEEDED(args->get_Uri(&uri)) && uri
                                            ? std::wstring(uri.get())
                                            : std::wstring();
            LeavePage(target, "newWindow", false, nullptr);
            return S_OK;
          })
          .Get(),
      &new_window_token_);

  // The YouTube live chat iframes (instruction 444). Without ICoreWebView2_4
  // (an old runtime) the frame script simply never runs.
  if (auto webview4 = webview_.try_query<ICoreWebView2_4>()) {
    webview4->add_FrameCreated(
        Callback<ICoreWebView2FrameCreatedEventHandler>(
            [this](ICoreWebView2* sender,
                   ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
              wil::com_ptr<ICoreWebView2Frame> frame;
              if (SUCCEEDED(args->get_Frame(frame.put())) && frame) {
                OnFrameCreated(frame.get());
              }
              return S_OK;
            })
            .Get(),
        &frame_created_token_);
  }

  FitToParent();
  controller_->put_IsVisible(visible_ ? TRUE : FALSE);
  if (visible_) {
    // Shown before the creation finished.
    controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
  webview_->Navigate(wurl.c_str());
  callback(true, "");
  return S_OK;
}

void ChildWebview::SetVisible(bool visible) {
  if (closed_ || !container_) {
    return;
  }
  visible_ = visible;
  if (visible) {
    FitToParent();
    SetWindowPos(container_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (controller_) {
      controller_->put_IsVisible(TRUE);
      controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
  } else {
    if (controller_) {
      controller_->put_IsVisible(FALSE);
    }
    ShowWindow(container_, SW_HIDE);
    if (IsWindow(flutter_view_)) {
      SetFocus(flutter_view_);
    }
  }
}

void ChildWebview::Focus() {
  if (closed_ || !controller_ || !visible_) {
    return;
  }
  controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void ChildWebview::PostMessage(const std::string& message) {
  if (closed_ || !webview_) {
    return;
  }
  webview_->PostWebMessageAsString(util::Utf16FromUtf8(message).c_str());
}

void ChildWebview::PostJson(const std::string& json) {
  if (closed_ || !webview_) {
    return;
  }
  webview_->PostWebMessageAsJson(util::Utf16FromUtf8(json).c_str());
}

void ChildWebview::LeavePage(const std::wstring& target, const char* what,
                             bool cancel,
                             ICoreWebView2NavigationStartingEventArgs* navigation) {
  if (cancel && navigation) {
    navigation->put_Cancel(TRUE);
  }
  const bool external = IsWebUrl(target);
  if (external) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
  if (listener_.on_navigation_blocked) {
    listener_.on_navigation_blocked(id_, what,
                                    util::Utf8FromUtf16(OriginOf(target)),
                                    external);
  }
}

void ChildWebview::SetFrameScript(const std::string& script) {
  frame_script_ = util::Utf16FromUtf8(script);
  if (closed_ || frame_script_.empty()) {
    return;
  }
  for (const auto& frame : frames_) {
    if (frame->loaded) {
      RunFrameScript(*frame);
    }
  }
}

void ChildWebview::OnFrameCreated(ICoreWebView2Frame* created) {
  if (closed_) {
    return;
  }
  auto frame2 = wil::com_ptr<ICoreWebView2Frame>(created)
                    .try_query<ICoreWebView2Frame2>();
  if (!frame2) {
    return;
  }
  auto entry = std::make_shared<Frame>();
  entry->frame = frame2;
  Frame* raw = entry.get();
  // The handlers are removed in DetachFrame (on Destroyed and on Close), so
  // [this] and [raw] are alive whenever they run.
  frame2->add_NavigationStarting(
      Callback<ICoreWebView2FrameNavigationStartingEventHandler>(
          [raw](ICoreWebView2Frame* sender,
                ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string uri;
            raw->url = SUCCEEDED(args->get_Uri(&uri)) && uri
                           ? std::wstring(uri.get())
                           : std::wstring();
            raw->loaded = false;
            return S_OK;
          })
          .Get(),
      &entry->navigation_token);
  frame2->add_DOMContentLoaded(
      Callback<ICoreWebView2FrameDOMContentLoadedEventHandler>(
          [this, raw](ICoreWebView2Frame* sender,
                      ICoreWebView2DOMContentLoadedEventArgs* args)
              -> HRESULT {
            raw->loaded = true;
            RunFrameScript(*raw);
            return S_OK;
          })
          .Get(),
      &entry->loaded_token);
  frame2->add_WebMessageReceived(
      Callback<ICoreWebView2FrameWebMessageReceivedEventHandler>(
          [this](ICoreWebView2Frame* sender,
                 ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string source;
            if (FAILED(args->get_Source(&source)) || !source ||
                !IsAllowedChatUrl(std::wstring(source.get()))) {
              return S_OK;
            }
            wil::unique_cotaskmem_string message;
            if (FAILED(args->TryGetWebMessageAsString(&message)) || !message) {
              return S_OK;
            }
            if (listener_.on_frame_message) {
              listener_.on_frame_message(
                  id_, util::Utf8FromUtf16(source.get()),
                  util::Utf8FromUtf16(message.get()));
            }
            return S_OK;
          })
          .Get(),
      &entry->message_token);
  frame2->add_Destroyed(
      Callback<ICoreWebView2FrameDestroyedEventHandler>(
          [this, raw](ICoreWebView2Frame* sender, IUnknown* args) -> HRESULT {
            RemoveFrame(raw);
            return S_OK;
          })
          .Get(),
      &entry->destroyed_token);
  frames_.push_back(entry);
}

void ChildWebview::RunFrameScript(const Frame& frame) {
  if (closed_ || frame_script_.empty() || !IsAllowedChatUrl(frame.url)) {
    return;
  }
  frame.frame->ExecuteScript(frame_script_.c_str(), nullptr);
}

void ChildWebview::RemoveFrame(const Frame* frame) {
  const auto it =
      std::find_if(frames_.begin(), frames_.end(),
                   [frame](const auto& entry) { return entry.get() == frame; });
  if (it == frames_.end()) {
    return;
  }
  // Keep the entry alive until its handlers are removed (we may be inside
  // one of them).
  const auto entry = *it;
  frames_.erase(it);
  DetachFrame(*entry);
}

void ChildWebview::DetachFrame(Frame& frame) {
  if (!frame.frame) {
    return;
  }
  frame.frame->remove_NavigationStarting(frame.navigation_token);
  frame.frame->remove_DOMContentLoaded(frame.loaded_token);
  frame.frame->remove_WebMessageReceived(frame.message_token);
  frame.frame->remove_Destroyed(frame.destroyed_token);
  frame.frame = nullptr;
}

void ChildWebview::OnParentSized() {
  if (!closed_) {
    FitToParent();
  }
}

void ChildWebview::OnParentMoved() {
  if (!closed_ && controller_) {
    controller_->NotifyParentWindowPositionChanged();
  }
}

void ChildWebview::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  const bool was_visible = visible_;
  visible_ = false;
  for (const auto& frame : frames_) {
    DetachFrame(*frame);
  }
  frames_.clear();
  if (webview_) {
    webview_->remove_WebMessageReceived(message_token_);
    webview_->remove_NavigationStarting(navigation_token_);
    webview_->remove_NewWindowRequested(new_window_token_);
    if (auto webview4 = webview_.try_query<ICoreWebView2_4>()) {
      webview4->remove_FrameCreated(frame_created_token_);
    }
    webview_ = nullptr;
  }
  if (controller_) {
    controller_->Close();
    controller_ = nullptr;
  }
  if (container_ && IsWindow(container_)) {
    DestroyWindow(container_);
  }
  container_ = nullptr;
  if (was_visible && IsWindow(flutter_view_)) {
    SetFocus(flutter_view_);
  }
}

void ChildWebview::FitToParent() {
  if (!container_) {
    return;
  }
  RECT client = {};
  GetClientRect(top_level_, &client);
  MoveWindow(container_, 0, 0, client.right - client.left,
             client.bottom - client.top, TRUE);
  if (controller_) {
    RECT bounds = {};
    GetClientRect(container_, &bounds);
    controller_->put_Bounds(bounds);
  }
}

void ChildWebview::OnWebMessage(
    ICoreWebView2WebMessageReceivedEventArgs* args) {
  wil::unique_cotaskmem_string source;
  if (FAILED(args->get_Source(&source)) || !source) {
    return;
  }
  const std::wstring wsource(source.get());
  if (wsource.compare(0, allowed_origin_.size(), allowed_origin_) != 0) {
    return;
  }
  wil::unique_cotaskmem_string message;
  if (FAILED(args->TryGetWebMessageAsString(&message)) || !message) {
    return;
  }
  const std::wstring wmessage(message.get());
  if (wmessage == kHideMessage) {
    SetVisible(false);
    if (listener_.on_hidden_by_page) {
      listener_.on_hidden_by_page(id_);
    }
    return;
  }
  if (listener_.on_message) {
    listener_.on_message(id_, util::Utf8FromUtf16(wsource),
                         util::Utf8FromUtf16(wmessage));
  }
}
