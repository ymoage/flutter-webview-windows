#include "child_webview.h"

#include <wrl.h>

#include "util/string_converter.h"

using namespace Microsoft::WRL;

namespace {

constexpr wchar_t kContainerClassName[] = L"PyokominChildWebviewHost";
// The page asks to be hidden with window.chrome.webview.postMessage("hide").
constexpr wchar_t kHideMessage[] = L"hide";
// kChat (ADR-0064): the only place the chat window may navigate to, and the
// only origin its messages are accepted from.
constexpr wchar_t kChatAllowedPrefix[] = L"https://www.youtube.com/live_chat";
constexpr wchar_t kChatOrigin[] = L"https://www.youtube.com/";

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

bool StartsWith(const std::wstring& text, const std::wstring& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

ChildWebview::ChildWebview(int64_t id, Role role, HWND top_level,
                           HWND flutter_view, Listener listener)
    : id_(id),
      role_(role),
      top_level_(top_level),
      flutter_view_(flutter_view),
      listener_(std::move(listener)) {}

ChildWebview::~ChildWebview() { Close(); }

void ChildWebview::Create(ICoreWebView2Environment* environment,
                          const std::string& host_name,
                          const std::string& folder, const std::string& url,
                          CreatedCallback callback) {
  EnsureContainerClass();
  // Starts empty; the owner places it (Place).
  container_ = CreateWindowEx(
      0, kContainerClassName, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
      0, 0, 0, 0, top_level_, nullptr, GetModuleHandle(nullptr), nullptr);
  if (!container_) {
    callback(false, "Creating the child window failed.");
    return;
  }
  // The Flutter view is a sibling. Without this it may paint over the child.
  // Not undone on close: in the Web UI a child window is always there
  // (ADR-0063), and the style only clips the view against its siblings, so it
  // changes nothing once they are gone.
  LONG_PTR view_style = GetWindowLongPtr(flutter_view_, GWL_STYLE);
  SetWindowLongPtr(flutter_view_, GWL_STYLE, view_style | WS_CLIPSIBLINGS);

  allowed_origin_ = role_ == Role::kPage
                        ? L"https://" + util::Utf16FromUtf8(host_name) + L"/"
                        : std::wstring(kChatOrigin);
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

  if (role_ == Role::kPage) {
    auto webview3 = webview_.try_query<ICoreWebView2_3>();
    if (!webview3 ||
        FAILED(webview3->SetVirtualHostNameToFolderMapping(
            whost.c_str(), wfolder.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
      callback(false, "Mapping the virtual host failed.");
      return S_OK;
    }
  } else {
    // A dropped URL would navigate the chat away (ADR-0064).
    auto controller4 = controller_.try_query<ICoreWebView2Controller4>();
    if (!controller4 || FAILED(controller4->put_AllowExternalDrop(FALSE))) {
      callback(false, "Turning off the drop failed.");
      return S_OK;
    }
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2* sender,
                   ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
              OnNavigationStarting(args);
              return S_OK;
            })
            .Get(),
        &navigation_token_);
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2* sender,
                   ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
              // Handled without a new window: nothing opens.
              args->put_Handled(TRUE);
              if (listener_.on_blocked) {
                listener_.on_blocked(id_, "newWindow");
              }
              return S_OK;
            })
            .Get(),
        &new_window_token_);
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
  controller_->add_GotFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [this](ICoreWebView2Controller* sender, IUnknown* args) -> HRESULT {
            if (listener_.on_focus) {
              listener_.on_focus(id_);
            }
            return S_OK;
          })
          .Get(),
      &focus_token_);

  ApplyControllerBounds();
  controller_->put_IsVisible(visible_ ? TRUE : FALSE);
  if (visible_ && role_ == Role::kPage) {
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
    SetWindowPos(container_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    if (controller_) {
      controller_->put_IsVisible(TRUE);
      // The chat is shown and hidden with the panel; it does not take the
      // focus by being shown.
      if (role_ == Role::kPage) {
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      }
    }
  } else {
    if (controller_) {
      controller_->put_IsVisible(FALSE);
    }
    ShowWindow(container_, SW_HIDE);
  }
}

void ChildWebview::Place(const RECT& bounds) {
  if (closed_ || !container_) {
    return;
  }
  MoveWindow(container_, bounds.left, bounds.top, bounds.right - bounds.left,
             bounds.bottom - bounds.top, TRUE);
  ApplyControllerBounds();
}

void ChildWebview::Focus() {
  if (closed_ || !controller_ || !visible_) {
    return;
  }
  controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
  visible_ = false;
  if (webview_) {
    webview_->remove_WebMessageReceived(message_token_);
    if (role_ == Role::kChat) {
      webview_->remove_NavigationStarting(navigation_token_);
      webview_->remove_NewWindowRequested(new_window_token_);
    }
    webview_ = nullptr;
  }
  if (controller_) {
    controller_->remove_GotFocus(focus_token_);
    controller_->Close();
    controller_ = nullptr;
  }
  if (container_ && IsWindow(container_)) {
    DestroyWindow(container_);
  }
  container_ = nullptr;
}

void ChildWebview::ApplyControllerBounds() {
  if (controller_ && container_) {
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
  if (!StartsWith(wsource, allowed_origin_)) {
    return;
  }
  wil::unique_cotaskmem_string message;
  if (FAILED(args->TryGetWebMessageAsString(&message)) || !message) {
    return;
  }
  const std::wstring wmessage(message.get());
  if (role_ == Role::kPage && wmessage == kHideMessage) {
    SetVisible(false);
    if (listener_.on_hidden_by_page) {
      listener_.on_hidden_by_page(id_);
    }
    return;
  }
  if (listener_.on_message) {
    listener_.on_message(id_, util::Utf8FromUtf16(wsource.c_str()),
                         util::Utf8FromUtf16(wmessage.c_str()));
  }
}

void ChildWebview::OnNavigationStarting(
    ICoreWebView2NavigationStartingEventArgs* args) {
  wil::unique_cotaskmem_string uri;
  if (SUCCEEDED(args->get_Uri(&uri)) && uri &&
      StartsWith(std::wstring(uri.get()), kChatAllowedPrefix)) {
    return;
  }
  args->put_Cancel(TRUE);
  if (listener_.on_blocked) {
    listener_.on_blocked(id_, "navigation");
  }
}
