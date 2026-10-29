#include "include/webview_windows/webview_windows_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "child_webview.h"
#include "util/string_converter.h"
#include "webview_bridge.h"
#include "webview_host.h"
#include "webview_platform.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

constexpr auto kMethodInitialize = "initialize";
constexpr auto kMethodDispose = "dispose";
constexpr auto kMethodInitializeEnvironment = "initializeEnvironment";
constexpr auto kMethodGetWebViewVersion = "getWebViewVersion";
// Pyokomin (ADR-0063): the child-window webview (one per app).
constexpr auto kMethodChildCreate = "childWebviewCreate";
constexpr auto kMethodChildSetVisible = "childWebviewSetVisible";
constexpr auto kMethodChildDispose = "childWebviewDispose";

constexpr auto kErrorCodeInvalidId = "invalid_id";
constexpr auto kErrorCodeEnvironmentCreationFailed =
    "environment_creation_failed";
constexpr auto kErrorCodeEnvironmentAlreadyInitialized =
    "environment_already_initialized";
constexpr auto kErrorCodeWebviewCreationFailed = "webview_creation_failed";
constexpr auto kErrorUnsupportedPlatform = "unsupported_platform";
constexpr auto kErrorCodeChildExists = "child_webview_exists";
constexpr auto kErrorCodeChildMissing = "child_webview_missing";
constexpr auto kErrorCodeChildCreationFailed = "child_webview_creation_failed";

template <typename T>
std::optional<T> GetOptionalValue(const flutter::EncodableMap& map,
                                  const std::string& key) {
  const auto it = map.find(flutter::EncodableValue(key));
  if (it != map.end()) {
    const auto val = std::get_if<T>(&it->second);
    if (val) {
      return *val;
    }
  }
  return std::nullopt;
}

class WebviewWindowsPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  WebviewWindowsPlugin(flutter::PluginRegistrarWindows* registrar,
                       flutter::TextureRegistrar* textures,
                       flutter::BinaryMessenger* messenger);

  virtual ~WebviewWindowsPlugin();

 private:
  std::unique_ptr<WebviewPlatform> platform_;
  std::unique_ptr<WebviewHost> webview_host_;
  std::unordered_map<int64_t, std::unique_ptr<WebviewBridge>> instances_;

  // design 93 / #24: guard for the async environment creation. While creation
  // is in flight, later requests must not start a second creation; they are
  // queued and flushed once the host is ready.
  bool host_creating_ = false;
  std::vector<std::function<void(bool success, std::string error)>>
      pending_host_callbacks_;

  // Pyokomin (ADR-0063): the child-window webview and its window proc
  // delegate on the top-level window (-1 = not registered).
  flutter::PluginRegistrarWindows* registrar_;
  std::shared_ptr<ChildWebview> child_;
  int child_proc_id_ = -1;
  // Set to false in the destructor. Completion handlers that outlive the
  // plugin must not answer Dart (the engine is gone, #194).
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  void CreateChildWebview(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void DisposeChildWebview();

  WNDCLASS window_class_ = {};
  flutter::TextureRegistrar* textures_;
  flutter::BinaryMessenger* messenger_;

  bool InitPlatform();

  // Ensure a WebviewHost exists: invoke callback immediately if present,
  // otherwise start async creation and invoke callback on completion. While a
  // creation is in flight, just queue the callback (no second creation).
  void EnsureHost(std::function<void(bool success, std::string error)> callback);
  // Start environment creation; on completion flush all pending_host_callbacks_.
  void StartHostCreation(std::optional<std::wstring> user_data_directory,
                         std::optional<std::wstring> browser_exe_path,
                         std::optional<std::string> arguments);

  // [profile] / [locked_down] (Pyokomin, ADR-0052): optional arguments of
  // "initialize" (profileName, inPrivate, lockedDown). Absent = as before.
  void CreateWebviewInstance(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>,
      std::optional<WebviewProfileOptions> profile, bool locked_down);
  // Continuation after the host is ready (create hwnd -> CreateWebview).
  void ContinueCreateWebviewInstance(
      std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
      std::optional<WebviewProfileOptions> profile, bool locked_down);
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
};

// static
void WebviewWindowsPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "io.jns.webview.win",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<WebviewWindowsPlugin>(
      registrar, registrar->texture_registrar(), registrar->messenger());

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

WebviewWindowsPlugin::WebviewWindowsPlugin(
    flutter::PluginRegistrarWindows* registrar,
    flutter::TextureRegistrar* textures, flutter::BinaryMessenger* messenger)
    : registrar_(registrar), textures_(textures), messenger_(messenger) {
  window_class_.lpszClassName = L"FlutterWebviewMessage";
  window_class_.lpfnWndProc = &DefWindowProc;
  RegisterClass(&window_class_);
}

WebviewWindowsPlugin::~WebviewWindowsPlugin() {
  // Pyokomin #194: the plugin is destroyed *during* engine shutdown, so the
  // messenger and the texture registrar are already dead here.
  //
  // Tell every bridge about it before destroying them, otherwise their
  // destructors call back into the engine and crash
  // (FlutterDesktopMessengerSetCallback -> access violation on every exit).
  //
  // The normal dispose path (Dart calls "dispose") erases a single instance
  // while the engine is alive and is unaffected: the flag stays true there,
  // so handlers and textures are still released properly.
  *alive_ = false;
  // Pyokomin (ADR-0063): close the child-window webview without touching the
  // registrar (the window proc delegate goes away with the registrar).
  if (child_) {
    child_->Close();
    child_ = nullptr;
  }

  for (auto& [texture_id, bridge] : instances_) {
    if (bridge) {
      bridge->MarkEngineGone();
    }
  }

  instances_.clear();
  UnregisterClass(window_class_.lpszClassName, nullptr);
}

void WebviewWindowsPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare(kMethodInitializeEnvironment) == 0) {
    if (webview_host_) {
      return result->Error(kErrorCodeEnvironmentAlreadyInitialized,
                           "The webview environment is already initialized");
    }

    if (!InitPlatform()) {
      return result->Error(kErrorUnsupportedPlatform,
                           "The platform is not supported");
    }

    const auto& map = std::get<flutter::EncodableMap>(*method_call.arguments());

    std::optional<std::wstring> browser_exe_wpath = std::nullopt;
    std::optional<std::string> browser_exe_path =
        GetOptionalValue<std::string>(map, "browserExePath");
    if (browser_exe_path) {
      browser_exe_wpath = util::Utf16FromUtf8(*browser_exe_path);
    }

    std::optional<std::wstring> user_data_wpath = std::nullopt;
    std::optional<std::string> user_data_path =
        GetOptionalValue<std::string>(map, "userDataPath");
    if (user_data_path) {
      user_data_wpath = util::Utf16FromUtf8(*user_data_path);
    } else {
      user_data_wpath = platform_->GetDefaultDataDirectory();
    }

    std::optional<std::string> additional_args =
        GetOptionalValue<std::string>(map, "additionalArguments");

    // design 93 / #24: respond from the environment creation completion
    // handler instead of blocking (webview_host_ was checked at the top of the
    // handler, so it is always null here).
    if (host_creating_) {
      // A lazy creation with default params may be in flight. To avoid using
      // the wrong custom arguments, return an error so the caller can retry.
      return result->Error(kErrorCodeEnvironmentCreationFailed,
                           "Environment creation is already in progress.");
    }

    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
        shared_result = std::move(result);
    pending_host_callbacks_.push_back(
        [shared_result](bool success, std::string error) {
          if (success) {
            shared_result->Success();
          } else {
            shared_result->Error(kErrorCodeEnvironmentCreationFailed, error);
          }
        });
    StartHostCreation(user_data_wpath, browser_exe_wpath, additional_args);
    return;
  }

  if (method_call.method_name().compare(kMethodGetWebViewVersion) == 0) {
    LPWSTR version_info = nullptr;
    auto hr =
        GetAvailableCoreWebView2BrowserVersionString(nullptr, &version_info);
    if (SUCCEEDED(hr) && version_info != nullptr) {
      return result->Success(
          flutter::EncodableValue(util::Utf8FromUtf16(version_info)));
    } else {
      return result->Success();
    }
  }

  if (method_call.method_name().compare(kMethodInitialize) == 0) {
    std::optional<WebviewProfileOptions> profile = std::nullopt;
    bool locked_down = false;
    if (const auto map =
            std::get_if<flutter::EncodableMap>(method_call.arguments())) {
      const auto profile_name = GetOptionalValue<std::string>(*map, "profileName");
      if (profile_name && !profile_name->empty()) {
        profile = WebviewProfileOptions{
            util::Utf16FromUtf8(*profile_name),
            GetOptionalValue<bool>(*map, "inPrivate").value_or(false)};
      }
      locked_down = GetOptionalValue<bool>(*map, "lockedDown").value_or(false);
    }
    return CreateWebviewInstance(std::move(result), profile, locked_down);
  }

  if (method_call.method_name().compare(kMethodChildCreate) == 0) {
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!map) {
      return result->Error(kErrorCodeChildCreationFailed, "No arguments.");
    }
    return CreateChildWebview(*map, std::move(result));
  }

  if (method_call.method_name().compare(kMethodChildSetVisible) == 0) {
    const auto visible = std::get_if<bool>(method_call.arguments());
    if (!child_ || !visible) {
      return result->Error(kErrorCodeChildMissing);
    }
    child_->SetVisible(*visible);
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodChildDispose) == 0) {
    DisposeChildWebview();
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodDispose) == 0) {
    if (const auto texture_id = std::get_if<int64_t>(method_call.arguments())) {
      const auto it = instances_.find(*texture_id);
      if (it != instances_.end()) {
        instances_.erase(it);
        return result->Success();
      }
    }
    return result->Error(kErrorCodeInvalidId);
  } else {
    result->NotImplemented();
  }
}

void WebviewWindowsPlugin::EnsureHost(
    std::function<void(bool success, std::string error)> callback) {
  if (webview_host_) {
    callback(true, "");
    return;
  }
  pending_host_callbacks_.push_back(std::move(callback));
  // If a creation is in flight, just queue (no second creation); it will be
  // flushed on completion.
  if (!host_creating_) {
    StartHostCreation(platform_->GetDefaultDataDirectory(), std::nullopt,
                      std::nullopt);
  }
}

void WebviewWindowsPlugin::StartHostCreation(
    std::optional<std::wstring> user_data_directory,
    std::optional<std::wstring> browser_exe_path,
    std::optional<std::string> arguments) {
  host_creating_ = true;
  WebviewHost::CreateAsync(
      platform_.get(),
      [this](std::unique_ptr<WebviewHost> host,
             std::unique_ptr<WebviewCreationError> error) {
        // This completion handler is delivered via the calling (UI) thread's
        // message loop.
        host_creating_ = false;
        std::string err;
        if (host) {
          webview_host_ = std::move(host);
        } else {
          err = error ? error->message : "Environment creation failed.";
        }
        const bool ok = (webview_host_ != nullptr);
        auto callbacks = std::move(pending_host_callbacks_);
        pending_host_callbacks_.clear();
        for (auto& cb : callbacks) {
          cb(ok, err);
        }
      },
      user_data_directory, browser_exe_path, arguments);
}

void WebviewWindowsPlugin::CreateWebviewInstance(
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result,
    std::optional<WebviewProfileOptions> profile, bool locked_down) {
  if (!InitPlatform()) {
    return result->Error(kErrorUnsupportedPlatform,
                         "The platform is not supported");
  }

  std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
      shared_result = std::move(result);
  // design 93 / #24: do not block on environment creation; run the rest after
  // the host is ready.
  EnsureHost([this, shared_result, profile, locked_down](bool success,
                                                         std::string error) {
    if (!success) {
      return shared_result->Error(kErrorCodeEnvironmentCreationFailed, error);
    }
    ContinueCreateWebviewInstance(shared_result, profile, locked_down);
  });
}

void WebviewWindowsPlugin::ContinueCreateWebviewInstance(
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
        shared_result,
    std::optional<WebviewProfileOptions> profile, bool locked_down) {
  auto hwnd =
      CreateWindowEx(0, window_class_.lpszClassName, L"", 0, 0, 0, 0, 0,
                     HWND_MESSAGE, nullptr, window_class_.hInstance, nullptr);

  webview_host_->CreateWebview(
      hwnd, true, true,
      [shared_result, this, locked_down](
          std::unique_ptr<Webview> webview,
          std::unique_ptr<WebviewCreationError> error) {
        if (!webview) {
          if (error) {
            return shared_result->Error(
                kErrorCodeWebviewCreationFailed,
                std::format(
                    "Creating the webview failed: {} (HRESULT: {:#010x})",
                    error->message, error->hr));
          }
          return shared_result->Error(kErrorCodeWebviewCreationFailed,
                                      "Creating the webview failed.");
        }

        // Pyokomin (ADR-0052): lock down before any page is loaded.
        if (locked_down) {
          webview->LockDown();
        }

        auto bridge = std::make_unique<WebviewBridge>(
            messenger_, textures_, platform_->graphics_context(),
            std::move(webview));
        auto texture_id = bridge->texture_id();
        instances_[texture_id] = std::move(bridge);

        auto response = flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue("textureId"),
             flutter::EncodableValue(texture_id)},
        });

        shared_result->Success(response);
      },
      profile);
}

void WebviewWindowsPlugin::CreateChildWebview(
    const flutter::EncodableMap& args,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (child_) {
    return result->Error(kErrorCodeChildExists);
  }
  const auto url = GetOptionalValue<std::string>(args, "url");
  const auto host_name = GetOptionalValue<std::string>(args, "hostName");
  const auto folder = GetOptionalValue<std::string>(args, "folder");
  if (!url || !host_name || !folder) {
    return result->Error(kErrorCodeChildCreationFailed,
                         "url, hostName and folder are required.");
  }
  if (!InitPlatform()) {
    return result->Error(kErrorUnsupportedPlatform,
                         "The platform is not supported");
  }
  const auto view = registrar_->GetView();
  const HWND flutter_view = view ? view->GetNativeWindow() : nullptr;
  const HWND top_level = flutter_view ? GetAncestor(flutter_view, GA_ROOT)
                                      : nullptr;
  if (!top_level) {
    return result->Error(kErrorCodeChildCreationFailed,
                         "The app window was not found.");
  }

  child_ = std::make_shared<ChildWebview>(top_level, flutter_view);
  child_proc_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
      [child = std::weak_ptr<ChildWebview>(child_)](
          HWND hwnd, UINT message, WPARAM wparam,
          LPARAM lparam) -> std::optional<LRESULT> {
        if (auto c = child.lock()) {
          return c->HandleTopLevelMessage(hwnd, message, wparam, lparam);
        }
        return std::nullopt;
      });

  std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
      shared_result = std::move(result);
  auto alive = alive_;
  EnsureHost([this, alive, shared_result, url, host_name, folder](
                 bool success, std::string error) {
    if (!*alive) {
      return;
    }
    if (!success || !child_) {
      return shared_result->Error(kErrorCodeEnvironmentCreationFailed, error);
    }
    child_->Create(webview_host_->environment(), *host_name, *folder, *url,
                   [alive, shared_result](bool ok, std::string message) {
                     if (!*alive) {
                       return;
                     }
                     if (ok) {
                       shared_result->Success();
                     } else {
                       shared_result->Error(kErrorCodeChildCreationFailed,
                                            message);
                     }
                   });
  });
}

void WebviewWindowsPlugin::DisposeChildWebview() {
  if (child_proc_id_ >= 0) {
    registrar_->UnregisterTopLevelWindowProcDelegate(child_proc_id_);
    child_proc_id_ = -1;
  }
  if (child_) {
    child_->Close();
    child_ = nullptr;
  }
}

bool WebviewWindowsPlugin::InitPlatform() {
  if (!platform_) {
    platform_ = std::make_unique<WebviewPlatform>();
  }
  return platform_->IsSupported();
}

}  // namespace

void WebviewWindowsPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  WebviewWindowsPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
