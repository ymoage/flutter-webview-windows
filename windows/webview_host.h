#pragma once

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wil/com.h>

#include <functional>

#include "graphics_context.h"
#include "webview.h"
#include "webview_platform.h"
#include "windows.ui.composition.h"

struct WebviewCreationError {
  HRESULT hr;
  std::string message;

  explicit WebviewCreationError(HRESULT hr, std::string message)
      : hr(hr), message(message) {}

  static std::unique_ptr<WebviewCreationError> create(
      HRESULT hr, const std::string message) {
    return std::make_unique<WebviewCreationError>(hr, message);
  }
};

class WebviewHost {
 public:
  typedef std::function<void(std::unique_ptr<Webview>,
                             std::unique_ptr<WebviewCreationError>)>
      WebviewCreationCallback;
  typedef std::function<void(wil::com_ptr<ICoreWebView2CompositionController>,
                             std::unique_ptr<WebviewCreationError>)>
      CompositionControllerCreationCallback;
  typedef std::function<void(wil::com_ptr<ICoreWebView2PointerInfo>,
                             std::unique_ptr<WebviewCreationError>)>
      PointerInfoCreationCallback;
  typedef std::function<void(std::unique_ptr<WebviewHost>,
                             std::unique_ptr<WebviewCreationError>)>
      HostCreationCallback;

  // ISSUES #24 / design 93: create the environment asynchronously.
  //
  // The old `Create` waited for CreateCoreWebView2EnvironmentWithOptions (an
  // async API) by blocking the calling thread on a future/promise. WebView2
  // delivers the completion handler through the calling thread's message loop,
  // so blocking that thread means the callback never arrives -> deadlock
  // (surfaced with WebView2 Runtime 151). Here the completion handler builds
  // the WebviewHost and passes it to [callback] (same style as CreateWebview).
  static void CreateAsync(
      WebviewPlatform* platform, HostCreationCallback callback,
      std::optional<std::wstring> user_data_directory = std::nullopt,
      std::optional<std::wstring> browser_exe_path = std::nullopt,
      std::optional<std::string> arguments = std::nullopt);

  // [profile] (Pyokomin, ADR-0052): create the webview in its own profile
  // instead of the default one. Needs ICoreWebView2Environment10.
  void CreateWebview(HWND hwnd, bool offscreen_only, bool owns_window,
                     WebviewCreationCallback callback,
                     std::optional<WebviewProfileOptions> profile = std::nullopt);

  void CreateWebViewPointerInfo(PointerInfoCreationCallback cb);

  // Pyokomin (ADR-0063): the child-window webview is created in the same
  // environment, so it shares the profile and the sign-in.
  ICoreWebView2Environment* environment() const { return webview_env_.get(); }

  winrt::com_ptr<ABI::Windows::UI::Composition::ICompositor> compositor()
      const {
    return compositor_;
  }

 private:
  winrt::com_ptr<ABI::Windows::UI::Composition::ICompositor> compositor_;
  wil::com_ptr<ICoreWebView2Environment3> webview_env_;

  WebviewHost(WebviewPlatform* platform,
              wil::com_ptr<ICoreWebView2Environment3> webview_env);
  void CreateWebViewCompositionController(
      HWND hwnd, std::optional<WebviewProfileOptions> profile,
      CompositionControllerCreationCallback cb);
};
