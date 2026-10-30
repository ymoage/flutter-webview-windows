#include "include/webview_windows/webview_windows_plugin.h"

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "child_focus.h"
#include "child_layout.h"
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
// Pyokomin (ADR-0063, ADR-0064): the child-window webviews (by id).
constexpr auto kMethodChildCreate = "childWebviewCreate";
constexpr auto kMethodChildSetVisible = "childWebviewSetVisible";
// ADR-0065: the chat area at the right edge of the window.
constexpr auto kMethodChildShowChat = "childWebviewShowChat";
constexpr auto kMethodChildSetChatWidth = "childWebviewSetChatWidth";
constexpr auto kMethodChildSetChatAccent = "childWebviewSetChatAccent";
constexpr auto kMethodChildDispose = "childWebviewDispose";
// Events to Dart, on their own channel.
constexpr auto kChildEventChannel = "io.jns.webview.win/child";
constexpr auto kMethodChildEvent = "childWebviewEvent";
constexpr auto kMethodChatWidthChanged = "childWebviewChatWidthChanged";
constexpr wchar_t kSplitterClassName[] = L"PyokominChildSplitter";
constexpr wchar_t kAccentClassName[] = L"PyokominChildAccent";
// The strip above the chat in the stream's color (logical pixels).
constexpr int kChatAccentDip = 3;

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

// Dart ints arrive as int32 or int64 depending on their size.
std::optional<int64_t> GetOptionalInt(const flutter::EncodableMap& map,
                                      const std::string& key) {
  const auto it = map.find(flutter::EncodableValue(key));
  if (it == map.end()) {
    return std::nullopt;
  }
  if (const auto v32 = std::get_if<int32_t>(&it->second)) {
    return *v32;
  }
  if (const auto v64 = std::get_if<int64_t>(&it->second)) {
    return *v64;
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

  // Pyokomin (ADR-0063, ADR-0064): the child-window webviews by id, and one
  // window proc delegate on the top-level window for all of them
  // (-1 = not registered).
  flutter::PluginRegistrarWindows* registrar_;
  std::map<int64_t, std::shared_ptr<ChildWebview>> children_;
  int child_proc_id_ = -1;
  HWND flutter_view_ = nullptr;
  HWND top_level_ = nullptr;
  // ADR-0065: the chat shown in the chat area (none = the area is closed),
  // the wanted width (logical pixels), the accent and the splitter window.
  std::optional<int64_t> shown_chat_;
  int chat_width_dip_ = kChatDefaultWidthDip;
  int last_chat_width_px_ = 0;
  COLORREF chat_accent_ = RGB(58, 63, 71);
  HWND splitter_ = nullptr;
  // The strip above the chat area in the stream's color.
  HWND accent_ = nullptr;
  bool splitter_dragging_ = false;
  int drag_start_x_ = 0;
  int drag_start_width_px_ = 0;
  // The child that got the keyboard focus last (kFocusRunner = none).
  int64_t last_focused_child_ = kFocusRunner;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      child_events_;
  // Set to false in the destructor. Completion handlers that outlive the
  // plugin must not answer Dart (the engine is gone, #194).
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  void CreateChildWebview(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  ChildWebview::Listener MakeChildListener();
  void SetChildVisible(int64_t id, bool visible);
  void ShowChat(std::optional<int64_t> id);
  // Places the page, the splitter and the shown chat side by side
  // (LayoutChildren). The only place that positions child windows.
  void Relayout();
  // The splitter and the accent strip (small windows of the plugin), made
  // and destroyed together.
  void EnsureSplitter();
  void DestroySplitter();
  static LRESULT CALLBACK AccentProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam);
  LRESULT HandleSplitterMessage(HWND hwnd, UINT message, WPARAM wparam,
                                LPARAM lparam);
  void DisposeChildWebview(int64_t id);
  // After a child was hidden or closed: give the focus to the shown page (if
  // a chat went away) or to the Flutter view.
  void FocusAfterChildGone(int64_t id, bool was_page);
  std::optional<LRESULT> HandleTopLevelMessage(UINT message, WPARAM wparam);
  void SendChildEvent(flutter::EncodableMap event);

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
  child_events_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          messenger, kChildEventChannel,
          &flutter::StandardMethodCodec::GetInstance());
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
  // Pyokomin (ADR-0063, ADR-0064): close every child-window webview without
  // touching the registrar (the window proc delegate goes away with the
  // registrar). Their listeners check alive_, so nothing reaches Dart.
  for (auto& [id, child] : children_) {
    child->Close();
  }
  children_.clear();
  DestroySplitter();

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
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    const auto id = map ? GetOptionalInt(*map, "id") : std::nullopt;
    const auto visible =
        map ? GetOptionalValue<bool>(*map, "visible") : std::nullopt;
    if (!id || !visible || children_.find(*id) == children_.end()) {
      return result->Error(kErrorCodeChildMissing);
    }
    if (children_.at(*id)->role() == ChildWebview::Role::kChat) {
      return result->Error(kErrorCodeChildMissing,
                           "A chat is shown with childWebviewShowChat.");
    }
    SetChildVisible(*id, *visible);
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodChildShowChat) == 0) {
    // {id: null} (or no id) closes the chat area.
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    const auto id = map ? GetOptionalInt(*map, "id") : std::nullopt;
    if (id) {
      const auto it = children_.find(*id);
      if (it == children_.end() ||
          it->second->role() != ChildWebview::Role::kChat) {
        return result->Error(kErrorCodeChildMissing);
      }
    }
    ShowChat(id);
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodChildSetChatWidth) == 0) {
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    const auto width = map ? GetOptionalInt(*map, "width") : std::nullopt;
    if (width && *width > 0) {
      chat_width_dip_ = static_cast<int>(*width);
      Relayout();
    }
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodChildSetChatAccent) == 0) {
    // 0xAARRGGBB (alpha is ignored).
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    const auto color = map ? GetOptionalInt(*map, "color") : std::nullopt;
    if (color) {
      const auto c = static_cast<uint32_t>(*color);
      chat_accent_ = RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
      if (accent_) {
        SetWindowLongPtr(accent_, GWLP_USERDATA,
                         static_cast<LONG_PTR>(chat_accent_));
        InvalidateRect(accent_, nullptr, TRUE);
      }
    }
    return result->Success();
  }

  if (method_call.method_name().compare(kMethodChildDispose) == 0) {
    const auto map = std::get_if<flutter::EncodableMap>(method_call.arguments());
    const auto id = map ? GetOptionalInt(*map, "id") : std::nullopt;
    if (id) {
      DisposeChildWebview(*id);
    }
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
  const auto id = GetOptionalInt(args, "id");
  const auto role_name = GetOptionalValue<std::string>(args, "role");
  const auto url = GetOptionalValue<std::string>(args, "url");
  const auto host_name =
      GetOptionalValue<std::string>(args, "hostName").value_or("");
  const auto folder = GetOptionalValue<std::string>(args, "folder").value_or("");
  if (!id || !role_name || !url) {
    return result->Error(kErrorCodeChildCreationFailed,
                         "id, role and url are required.");
  }
  ChildWebview::Role role;
  if (*role_name == "page") {
    role = ChildWebview::Role::kPage;
    if (host_name.empty() || folder.empty()) {
      return result->Error(kErrorCodeChildCreationFailed,
                           "A page needs hostName and folder.");
    }
  } else if (*role_name == "chat") {
    role = ChildWebview::Role::kChat;
  } else {
    return result->Error(kErrorCodeChildCreationFailed, "Unknown role.");
  }
  if (children_.find(*id) != children_.end()) {
    return result->Error(kErrorCodeChildExists);
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
  flutter_view_ = flutter_view;
  top_level_ = top_level;

  auto child = std::make_shared<ChildWebview>(*id, role, top_level,
                                              flutter_view, MakeChildListener());
  children_[*id] = child;
  if (child_proc_id_ < 0) {
    auto proc_alive = alive_;
    child_proc_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
        [this, proc_alive](HWND hwnd, UINT message, WPARAM wparam,
                           LPARAM lparam) -> std::optional<LRESULT> {
          if (!*proc_alive) {
            return std::nullopt;
          }
          return HandleTopLevelMessage(message, wparam);
        });
  }

  std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>
      shared_result = std::move(result);
  auto alive = alive_;
  // Hold the child that was asked for: if it is disposed (and maybe created
  // again under the same id) while the environment is still being created,
  // this completion must not create the newer one a second time.
  std::weak_ptr<ChildWebview> weak_child = child;
  const int64_t child_id = *id;
  EnsureHost([this, alive, shared_result, weak_child, child_id, url, host_name,
              folder](bool success, std::string error) {
    if (!*alive) {
      return;
    }
    auto c = weak_child.lock();
    const auto it = children_.find(child_id);
    if (!c || it == children_.end() || it->second != c) {
      return shared_result->Error(kErrorCodeChildCreationFailed,
                                  "Disposed before the environment was ready.");
    }
    if (!success) {
      return shared_result->Error(kErrorCodeEnvironmentCreationFailed, error);
    }
    c->Create(webview_host_->environment(), host_name, folder, *url,
              [alive, shared_result](bool ok, std::string message) {
                if (!*alive) {
                  return;
                }
                if (ok) {
                  shared_result->Success();
                } else {
                  shared_result->Error(kErrorCodeChildCreationFailed, message);
                }
              });
    // The container exists now (the controller follows later): place it.
    Relayout();
  });
}

ChildWebview::Listener WebviewWindowsPlugin::MakeChildListener() {
  auto alive = alive_;
  ChildWebview::Listener listener;
  listener.on_focus = [this, alive](int64_t id) {
    if (*alive) {
      last_focused_child_ = id;
    }
  };
  listener.on_hidden_by_page = [this, alive](int64_t id) {
    if (!*alive) {
      return;
    }
    FocusAfterChildGone(id, true);
    // The chat area goes with the page.
    Relayout();
    SendChildEvent(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("kind"), flutter::EncodableValue("hidden")},
    });
  };
  listener.on_message = [this, alive](int64_t id, const std::string& source,
                                      const std::string& message) {
    if (!*alive) {
      return;
    }
    SendChildEvent(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("kind"), flutter::EncodableValue("message")},
        {flutter::EncodableValue("source"), flutter::EncodableValue(source)},
        {flutter::EncodableValue("message"), flutter::EncodableValue(message)},
    });
  };
  listener.on_blocked = [this, alive](int64_t id, const std::string& what) {
    if (!*alive) {
      return;
    }
    SendChildEvent(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("kind"), flutter::EncodableValue("blocked")},
        {flutter::EncodableValue("what"), flutter::EncodableValue(what)},
    });
  };
  return listener;
}

void WebviewWindowsPlugin::SetChildVisible(int64_t id, bool visible) {
  const auto child = children_.at(id);
  const bool was_visible = child->visible();
  child->SetVisible(visible);
  // The chat area is shown only with the page.
  Relayout();
  if (!visible && was_visible) {
    FocusAfterChildGone(id, child->role() == ChildWebview::Role::kPage);
  }
}

void WebviewWindowsPlugin::ShowChat(std::optional<int64_t> id) {
  shown_chat_ = id;
  Relayout();
}

void WebviewWindowsPlugin::Relayout() {
  if (!top_level_ || !IsWindow(top_level_)) {
    return;
  }
  std::shared_ptr<ChildWebview> page;
  for (auto& [id, child] : children_) {
    if (child->role() == ChildWebview::Role::kPage) {
      page = child;
      break;
    }
  }
  std::shared_ptr<ChildWebview> chat;
  if (shown_chat_) {
    const auto it = children_.find(*shown_chat_);
    if (it != children_.end()) {
      chat = it->second;
    }
  }
  const bool open = page && page->visible() && chat;

  RECT client = {};
  GetClientRect(top_level_, &client);
  UINT dpi = GetDpiForWindow(top_level_);
  if (dpi == 0) {
    dpi = 96;
  }
  const ChildLayout layout =
      LayoutChildren(client.right - client.left, client.bottom - client.top,
                     chat_width_dip_, dpi, open);
  const auto to_rect = [](const LayoutRect& r) {
    return RECT{r.x, r.y, r.x + r.width, r.y + r.height};
  };

  if (page) {
    page->Place(to_rect(layout.page));
  }
  if (open) {
    last_chat_width_px_ = layout.chat.width;
    EnsureSplitter();
    if (splitter_) {
      // Above the Flutter view (a full-size sibling); otherwise the view
      // takes the mouse and both paint the strip.
      SetWindowPos(splitter_, HWND_TOP, layout.splitter.x, layout.splitter.y,
                   layout.splitter.width, layout.splitter.height,
                   SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
  } else if (splitter_) {
    ShowWindow(splitter_, SW_HIDE);
  }
  // The accent strip takes the top of the chat area; the chat goes below it.
  const int accent = open ? DipToPx(kChatAccentDip, dpi) : 0;
  if (open && accent_) {
    SetWindowPos(accent_, HWND_TOP, layout.chat.x, layout.chat.y,
                 layout.chat.width, accent, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  } else if (accent_) {
    ShowWindow(accent_, SW_HIDE);
  }
  LayoutRect chat_rect = layout.chat;
  chat_rect.y += accent;
  chat_rect.height = chat_rect.height > accent ? chat_rect.height - accent : 0;
  // The shown chat goes to the chat area; the others are hidden but alive.
  for (auto& [id, child] : children_) {
    if (child->role() != ChildWebview::Role::kChat) {
      continue;
    }
    if (open && child == chat) {
      child->Place(to_rect(chat_rect));
      if (!child->visible()) {
        child->SetVisible(true);
      }
    } else if (child->visible()) {
      child->SetVisible(false);
      if (last_focused_child_ == id) {
        FocusAfterChildGone(id, false);
      }
    }
  }
}

void WebviewWindowsPlugin::EnsureSplitter() {
  if (splitter_ || !top_level_) {
    return;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASS window_class = {};
    window_class.lpszClassName = kSplitterClassName;
    window_class.lpfnWndProc = &WebviewWindowsPlugin::SplitterProc;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_SIZEWE);
    window_class.hbrBackground = CreateSolidBrush(RGB(58, 63, 71));
    RegisterClass(&window_class);
    registered = true;
  }
  splitter_ = CreateWindowEx(0, kSplitterClassName, L"",
                             WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                             top_level_, nullptr, GetModuleHandle(nullptr),
                             nullptr);
  if (splitter_) {
    SetWindowLongPtr(splitter_, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(this));
  }

  static bool accent_registered = false;
  if (!accent_registered) {
    WNDCLASS window_class = {};
    window_class.lpszClassName = kAccentClassName;
    window_class.lpfnWndProc = &WebviewWindowsPlugin::AccentProc;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&window_class);
    accent_registered = true;
  }
  accent_ = CreateWindowEx(0, kAccentClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
                           0, 0, 0, 0, top_level_, nullptr,
                           GetModuleHandle(nullptr), nullptr);
  if (accent_) {
    SetWindowLongPtr(accent_, GWLP_USERDATA,
                     static_cast<LONG_PTR>(chat_accent_));
  }
}

// static
LRESULT CALLBACK WebviewWindowsPlugin::AccentProc(HWND hwnd, UINT message,
                                                  WPARAM wparam,
                                                  LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      // The color is kept in GWLP_USERDATA (no pointer to the plugin).
      PAINTSTRUCT paint;
      HDC dc = BeginPaint(hwnd, &paint);
      HBRUSH brush = CreateSolidBrush(
          static_cast<COLORREF>(GetWindowLongPtr(hwnd, GWLP_USERDATA)));
      FillRect(dc, &paint.rcPaint, brush);
      DeleteObject(brush);
      EndPaint(hwnd, &paint);
      return 0;
    }
  }
  return DefWindowProc(hwnd, message, wparam, lparam);
}

void WebviewWindowsPlugin::DestroySplitter() {
  if (splitter_ && IsWindow(splitter_)) {
    // Detach first: no message may reach this plugin any more.
    SetWindowLongPtr(splitter_, GWLP_USERDATA, 0);
    DestroyWindow(splitter_);
  }
  splitter_ = nullptr;
  splitter_dragging_ = false;
  if (accent_ && IsWindow(accent_)) {
    DestroyWindow(accent_);
  }
  accent_ = nullptr;
}

// static
LRESULT CALLBACK WebviewWindowsPlugin::SplitterProc(HWND hwnd, UINT message,
                                                    WPARAM wparam,
                                                    LPARAM lparam) {
  auto plugin = reinterpret_cast<WebviewWindowsPlugin*>(
      GetWindowLongPtr(hwnd, GWLP_USERDATA));
  if (plugin) {
    return plugin->HandleSplitterMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProc(hwnd, message, wparam, lparam);
}

LRESULT WebviewWindowsPlugin::HandleSplitterMessage(HWND hwnd, UINT message,
                                                    WPARAM wparam,
                                                    LPARAM lparam) {
  switch (message) {
    case WM_LBUTTONDOWN: {
      POINT point = {};
      GetCursorPos(&point);
      drag_start_x_ = point.x;
      drag_start_width_px_ = last_chat_width_px_;
      splitter_dragging_ = true;
      SetCapture(hwnd);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!splitter_dragging_) {
        break;
      }
      POINT point = {};
      GetCursorPos(&point);
      UINT dpi = GetDpiForWindow(hwnd);
      if (dpi == 0) {
        dpi = 96;
      }
      // Dragging to the left widens the chat area.
      const int width_px = drag_start_width_px_ - (point.x - drag_start_x_);
      chat_width_dip_ = PxToDip(width_px > 0 ? width_px : 0, dpi);
      Relayout();
      // Keep the wish inside the limits, so that dragging back reacts at once.
      chat_width_dip_ = PxToDip(last_chat_width_px_, dpi);
      return 0;
    }
    case WM_LBUTTONUP:
      if (splitter_dragging_) {
        ReleaseCapture();
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (splitter_dragging_) {
        splitter_dragging_ = false;
        // Only when the drag ends: the app keeps the width in its settings.
        if (*alive_ && child_events_) {
          child_events_->InvokeMethod(
              kMethodChatWidthChanged,
              std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
                  {flutter::EncodableValue("width"),
                   flutter::EncodableValue(chat_width_dip_)},
              }));
        }
      }
      return 0;
  }
  return DefWindowProc(hwnd, message, wparam, lparam);
}

void WebviewWindowsPlugin::DisposeChildWebview(int64_t id) {
  const auto it = children_.find(id);
  if (it == children_.end()) {
    return;
  }
  const auto child = it->second;
  children_.erase(it);
  const bool was_visible = child->visible();
  const bool is_page = child->role() == ChildWebview::Role::kPage;
  child->Close();
  if (shown_chat_ == id) {
    // Closing the shown chat closes the chat area.
    shown_chat_.reset();
  }
  Relayout();
  if (was_visible) {
    FocusAfterChildGone(id, is_page);
  }
  if (last_focused_child_ == id) {
    last_focused_child_ = kFocusRunner;
  }
  if (children_.empty()) {
    DestroySplitter();
    if (child_proc_id_ >= 0) {
      registrar_->UnregisterTopLevelWindowProcDelegate(child_proc_id_);
      child_proc_id_ = -1;
    }
  }
}

void WebviewWindowsPlugin::FocusAfterChildGone(int64_t id, bool was_page) {
  if (!was_page) {
    for (auto& [other_id, other] : children_) {
      if (other_id != id && other->role() == ChildWebview::Role::kPage &&
          other->visible()) {
        other->Focus();
        return;
      }
    }
  }
  if (flutter_view_ && IsWindow(flutter_view_)) {
    SetFocus(flutter_view_);
  }
}

std::optional<LRESULT> WebviewWindowsPlugin::HandleTopLevelMessage(
    UINT message, WPARAM wparam) {
  switch (message) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        Relayout();
      }
      // The runner still resizes the Flutter view.
      return std::nullopt;
    case WM_DPICHANGED:
      // The runner resizes the window too (WM_SIZE follows).
      Relayout();
      return std::nullopt;
    case WM_MOVE:
      for (auto& [id, child] : children_) {
        child->OnParentMoved();
      }
      return std::nullopt;
  }
  std::vector<ChildFocusCandidate> candidates;
  for (auto& [id, child] : children_) {
    candidates.push_back(
        {id, child->visible(), child->role() == ChildWebview::Role::kPage});
  }
  const int64_t target =
      DecideActivateFocus(message, wparam, last_focused_child_, candidates);
  if (target == kFocusRunner) {
    return std::nullopt;
  }
  // Taken over: the runner would give the focus to the Flutter view.
  children_.at(target)->Focus();
  return 0;
}

void WebviewWindowsPlugin::SendChildEvent(flutter::EncodableMap event) {
  if (!*alive_ || !child_events_) {
    return;
  }
  child_events_->InvokeMethod(
      kMethodChildEvent,
      std::make_unique<flutter::EncodableValue>(std::move(event)));
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
