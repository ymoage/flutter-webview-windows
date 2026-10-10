#pragma once

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>

#include <memory>

#include "graphics_context.h"
#include "texture_bridge.h"
#include "webview.h"

class WebviewBridge {
 public:
  WebviewBridge(flutter::BinaryMessenger* messenger,
                flutter::TextureRegistrar* texture_registrar,
                GraphicsContext* graphics_context,
                std::unique_ptr<Webview> webview);
  ~WebviewBridge();

  // Pyokomin #194: tell this bridge that the Flutter engine is already gone.
  //
  // The destructor normally talks back to the engine (clearing the method
  // call handler and unregistering the texture). That is correct while the
  // engine is alive, but during engine shutdown the messenger is already
  // dead and those calls crash inside
  // FlutterDesktopMessengerSetCallback (access violation, every exit).
  //
  // Note that the engine's own C++ wrapper guards the *send* path with
  // FlutterDesktopMessengerIsAvailable() but does NOT guard the path that
  // clears a handler, so the plugin has to avoid calling it.
  void MarkEngineGone() { engine_alive_ = false; }

  TextureBridge* texture_bridge() const { return texture_bridge_.get(); }

  int64_t texture_id() const { return texture_id_; }

 private:
  std::unique_ptr<flutter::TextureVariant> flutter_texture_;
  std::unique_ptr<TextureBridge> texture_bridge_;
  std::unique_ptr<Webview> webview_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;

  flutter::TextureRegistrar* texture_registrar_;
  int64_t texture_id_;

  // Pyokomin #194: false once the engine is shutting down. See MarkEngineGone().
  bool engine_alive_ = true;

  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void RegisterEventHandlers();

  template <typename T>
  void EmitEvent(const T& value) {
    if (event_sink_) {
      event_sink_->Success(value);
    }
  }

  void OnPermissionRequested(
      const std::string& url, WebviewPermissionKind permissionKind,
      bool is_user_initiated,
      Webview::WebviewPermissionRequestedCompleter completer);
};
