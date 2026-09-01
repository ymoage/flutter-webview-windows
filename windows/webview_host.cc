#include "webview_host.h"

#include <wrl.h>

#include <iostream>

#include "util/rohelper.h"

using namespace Microsoft::WRL;

// static
void WebviewHost::CreateAsync(
    WebviewPlatform* platform, HostCreationCallback callback,
    std::optional<std::wstring> user_data_directory,
    std::optional<std::wstring> browser_exe_path,
    std::optional<std::string> arguments) {
  wil::com_ptr<CoreWebView2EnvironmentOptions> opts;
  if (arguments.has_value()) {
    opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    std::wstring warguments(arguments.value().begin(), arguments.value().end());
    opts->put_AdditionalBrowserArguments(warguments.c_str());
  }

  auto hr = CreateCoreWebView2EnvironmentWithOptions(
      browser_exe_path.has_value() ? browser_exe_path->c_str() : nullptr,
      user_data_directory.has_value() ? user_data_directory->c_str() : nullptr,
      opts.get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [platform, callback](HRESULT r,
                               ICoreWebView2Environment* env) -> HRESULT {
            if ((SUCCEEDED(r) || r == RPC_E_CHANGED_MODE) && env) {
              wil::com_ptr<ICoreWebView2Environment> env_ptr(env);
              auto webview_env3 = env_ptr.try_query<ICoreWebView2Environment3>();
              if (webview_env3) {
                callback(std::unique_ptr<WebviewHost>(new WebviewHost(
                             platform, std::move(webview_env3))),
                         nullptr);
                return S_OK;
              }
              callback(nullptr,
                       WebviewCreationError::create(
                           r, "ICoreWebView2Environment3 is not available."));
              return S_OK;
            }
            callback(nullptr,
                     WebviewCreationError::create(
                         r,
                         "CreateCoreWebView2EnvironmentWithOptions completion "
                         "handler reported failure."));
            return S_OK;
          })
          .Get());

  // If the call itself fails synchronously the completion handler never runs.
  if (FAILED(hr)) {
    callback(nullptr,
             WebviewCreationError::create(
                 hr, "CreateCoreWebView2EnvironmentWithOptions failed."));
  }
}

WebviewHost::WebviewHost(WebviewPlatform* platform,
                         wil::com_ptr<ICoreWebView2Environment3> webview_env)
    : webview_env_(webview_env) {
  compositor_ = platform->graphics_context()->CreateCompositor();
}

void WebviewHost::CreateWebview(HWND hwnd, bool offscreen_only,
                                bool owns_window,
                                WebviewCreationCallback callback) {
  CreateWebViewCompositionController(
      hwnd, [=, self = this](
                wil::com_ptr<ICoreWebView2CompositionController> controller,
                std::unique_ptr<WebviewCreationError> error) {
        if (controller) {
          std::unique_ptr<Webview> webview(new Webview(
              std::move(controller), self, hwnd, owns_window, offscreen_only));
          callback(std::move(webview), nullptr);
        } else {
          callback(nullptr, std::move(error));
        }
      });
}

void WebviewHost::CreateWebViewPointerInfo(PointerInfoCreationCallback callback) {

  ICoreWebView2PointerInfo *pointer;
  auto hr = webview_env_->CreateCoreWebView2PointerInfo(&pointer);

  if (FAILED(hr)) {
    callback(nullptr, WebviewCreationError::create(hr, "CreateWebViewPointerInfo failed."));
  } else if (SUCCEEDED(hr)) {
    callback(std::move(wil::com_ptr<ICoreWebView2PointerInfo>(pointer)), nullptr);
  }
}

void WebviewHost::CreateWebViewCompositionController(
    HWND hwnd, CompositionControllerCreationCallback callback) {
  auto hr = webview_env_->CreateCoreWebView2CompositionController(
      hwnd,
      Callback<
          ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
          [callback](HRESULT hr,
                     ICoreWebView2CompositionController* compositionController)
              -> HRESULT {
            if (SUCCEEDED(hr)) {
              callback(
                  std::move(wil::com_ptr<ICoreWebView2CompositionController>(
                      compositionController)),
                  nullptr);
            } else {
              callback(nullptr, WebviewCreationError::create(
                                    hr,
                                    "CreateCoreWebView2CompositionController "
                                    "completion handler failed."));
            }

            return S_OK;
          })
          .Get());

  if (FAILED(hr)) {
    callback(nullptr,
             WebviewCreationError::create(
                 hr, "CreateCoreWebView2CompositionController failed."));
  }
}
