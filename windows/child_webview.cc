#include "child_webview.h"

#include <wrl.h>

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
  if (webview_) {
    webview_->remove_WebMessageReceived(message_token_);
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
