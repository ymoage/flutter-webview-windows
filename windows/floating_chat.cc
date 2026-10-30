#include "floating_chat.h"

#include <wrl.h>

#include "child_chat_guard.h"
#include "util/string_converter.h"

using namespace Microsoft::WRL;

namespace {

constexpr wchar_t kWindowClassName[] = L"PyokominFloatingChat";
// A fixed title: the stream's title is not shown (it is the user's data).
constexpr wchar_t kWindowTitle[] = L"Pyokomin - YouTube chat";
// Logical pixels.
constexpr int kDefaultWidthDip = 420;
constexpr int kDefaultHeightDip = 720;

void EnsureWindowClass(WNDPROC proc) {
  static bool registered = false;
  if (registered) {
    return;
  }
  WNDCLASS window_class = {};
  window_class.lpszClassName = kWindowClassName;
  window_class.lpfnWndProc = proc;
  window_class.hInstance = GetModuleHandle(nullptr);
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(101));
  RegisterClass(&window_class);
  registered = true;
}

}  // namespace

FloatingChat::FloatingChat(int64_t id, Listener listener)
    : id_(id), listener_(std::move(listener)) {}

FloatingChat::~FloatingChat() { Close(); }

void FloatingChat::Create(ICoreWebView2Environment* environment,
                          const std::string& url, CreatedCallback callback) {
  EnsureWindowClass(&FloatingChat::WindowProc);
  // Sized for the system dpi; WM_DPICHANGED fixes it on another monitor.
  const UINT dpi = GetDpiForSystem();
  window_ = CreateWindowEx(
      0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, MulDiv(kDefaultWidthDip, dpi, 96),
      MulDiv(kDefaultHeightDip, dpi, 96), nullptr, nullptr,
      GetModuleHandle(nullptr), nullptr);
  if (!window_) {
    callback(false, "Creating the window failed.");
    return;
  }
  SetWindowLongPtr(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(window_, SW_SHOWNORMAL);

  const std::wstring wurl = util::Utf16FromUtf8(url);
  auto hr = environment->CreateCoreWebView2Controller(
      window_,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [self = shared_from_this(), wurl, callback](
              HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            return self->OnControllerCreated(result, controller, wurl,
                                             callback);
          })
          .Get());
  if (FAILED(hr)) {
    callback(false, "CreateCoreWebView2Controller could not be started.");
  }
}

HRESULT FloatingChat::OnControllerCreated(HRESULT result,
                                          ICoreWebView2Controller* controller,
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
  // A dropped URL would navigate the chat away.
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
              wil::unique_cotaskmem_string uri;
              const std::wstring target =
                  SUCCEEDED(args->get_Uri(&uri)) && uri ? uri.get() : L"";
              listener_.on_blocked(id_, "newWindow",
                                   util::Utf8FromUtf16(OriginOf(target)));
            }
            return S_OK;
          })
          .Get(),
      &new_window_token_);

  FitController();
  controller_->put_IsVisible(TRUE);
  if (GetForegroundWindow() == window_) {
    FocusWebview();
  }
  webview_->Navigate(wurl.c_str());
  callback(true, "");
  return S_OK;
}

void FloatingChat::BringToFront() {
  if (closed_ || !window_) {
    return;
  }
  if (IsIconic(window_)) {
    ShowWindow(window_, SW_RESTORE);
  }
  SetForegroundWindow(window_);
}

void FloatingChat::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (webview_) {
    webview_->remove_NavigationStarting(navigation_token_);
    webview_->remove_NewWindowRequested(new_window_token_);
    webview_ = nullptr;
  }
  if (controller_) {
    controller_->Close();
    controller_ = nullptr;
  }
  if (window_ && IsWindow(window_)) {
    // Detach first: no message may reach this object any more.
    SetWindowLongPtr(window_, GWLP_USERDATA, 0);
    DestroyWindow(window_);
  }
  window_ = nullptr;
}

// static
LRESULT CALLBACK FloatingChat::WindowProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
  auto self =
      reinterpret_cast<FloatingChat*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (self) {
    return self->HandleMessage(message, wparam, lparam);
  }
  return DefWindowProc(hwnd, message, wparam, lparam);
}

LRESULT FloatingChat::HandleMessage(UINT message, WPARAM wparam,
                                    LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        FitController();
      }
      return 0;
    case WM_MOVE:
      if (controller_) {
        controller_->NotifyParentWindowPositionChanged();
      }
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wparam) != WA_INACTIVE) {
        FocusWebview();
        return 0;
      }
      break;
    case WM_SETFOCUS:
      FocusWebview();
      return 0;
    case WM_DPICHANGED: {
      // Use the size Windows suggests for the new dpi.
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left,
                   suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_CLOSE: {
      // The owner disposes it (and so the window). Keep the default
      // DestroyWindow from running behind its back. Held here so that the
      // owner dropping its reference does not free this object mid-call.
      auto keep_alive = shared_from_this();
      if (listener_.on_closed) {
        listener_.on_closed(id_);
      } else {
        Close();
      }
      return 0;
    }
  }
  return DefWindowProc(window_, message, wparam, lparam);
}

void FloatingChat::FitController() {
  if (controller_ && window_) {
    RECT bounds = {};
    GetClientRect(window_, &bounds);
    controller_->put_Bounds(bounds);
  }
}

void FloatingChat::FocusWebview() {
  if (!closed_ && controller_) {
    controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
}

void FloatingChat::OnNavigationStarting(
    ICoreWebView2NavigationStartingEventArgs* args) {
  wil::unique_cotaskmem_string uri;
  const std::wstring target = SUCCEEDED(args->get_Uri(&uri)) && uri
                                  ? std::wstring(uri.get())
                                  : std::wstring();
  if (IsAllowedChatUrl(target)) {
    return;
  }
  args->put_Cancel(TRUE);
  if (listener_.on_blocked) {
    listener_.on_blocked(id_, "navigation",
                         util::Utf8FromUtf16(OriginOf(target)));
  }
}
