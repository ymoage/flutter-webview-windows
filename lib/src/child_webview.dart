import 'dart:async';

import 'package:flutter/services.dart';

const MethodChannel _channel = MethodChannel('io.jns.webview.win');
const MethodChannel _eventChannel = MethodChannel('io.jns.webview.win/child');

/// An event from the child-window page or a floating chat.
abstract class ChildWebviewEvent {
  const ChildWebviewEvent();
}

/// The page [id] hid itself (`postMessage('hide')`).
class ChildWebviewHidden extends ChildWebviewEvent {
  const ChildWebviewHidden(this.id);
  final int id;
}

/// A web message from the page [id], from its own origin. [source] is the
/// sender's URL.
class ChildWebviewMessage extends ChildWebviewEvent {
  const ChildWebviewMessage(this.id, this.source, this.message);
  final int id;
  final String source;
  final String message;
}

/// A web message from a YouTube live chat iframe of the page [id]
/// (instruction 444; see [ChildWebview.setFrameScript]). [source] is the
/// frame's URL.
class ChildWebviewFrameMessage extends ChildWebviewEvent {
  const ChildWebviewFrameMessage(this.id, this.source, this.message);
  final int id;
  final String source;
  final String message;
}

/// A top-level navigation (`navigation`) or a new window (`newWindow`) of the
/// page [id] went outside its own origin and was cancelled (instruction
/// 445). [external] tells whether it was handed to the default browser
/// (http and https only). Only the [origin] of the target is passed on.
class ChildWebviewNavigationBlocked extends ChildWebviewEvent {
  const ChildWebviewNavigationBlocked(
      this.id, this.what, this.origin, this.external);
  final int id;
  final String what;
  final String origin;
  final bool external;
}

/// The user closed the floating chat [id] (it is already disposed).
class FloatingChatClosed extends ChildWebviewEvent {
  const FloatingChatClosed(this.id);
  final int id;
}

/// A navigation (`navigation`) or a new window (`newWindow`) of the floating
/// chat [id] was cancelled. Only the [origin] of the target is passed on
/// (up to the first '/' after "://"; empty when unknown), never its path.
class FloatingChatBlocked extends ChildWebviewEvent {
  const FloatingChatBlocked(this.id, this.what, [this.origin = '']);
  final int id;
  final String what;
  final String origin;
}

/// Pyokomin (ADR-0063, ADR-0067, #298): the app's page hosted in a real child
/// window of the app window (a plain WebView2 controller, not the
/// texture-based one), addressed by an id.
///
/// It fills the client area of the app window and shares the environment
/// (profile and sign-in) with the other webviews. When the app window
/// becomes active, the focus goes back to the shown page. The page can hide
/// itself with `window.chrome.webview.postMessage('hide')` (only from
/// `https://<hostName>/`).
class ChildWebview {
  ChildWebview._();

  static StreamController<ChildWebviewEvent>? _events;

  /// Events of the page and the floating chats.
  static Stream<ChildWebviewEvent> get events {
    final existing = _events;
    if (existing != null) return existing.stream;
    final controller = StreamController<ChildWebviewEvent>.broadcast();
    _events = controller;
    _eventChannel.setMethodCallHandler((call) async {
      final event = _parseEvent(call.method, call.arguments);
      if (event != null) controller.add(event);
    });
    return controller.stream;
  }

  static ChildWebviewEvent? _parseEvent(String method, Object? arguments) {
    if (arguments is! Map) return null;
    final id = arguments['id'];
    if (id is! int) return null;
    final kind = arguments['kind'];
    if (method == 'childWebviewEvent') {
      if (kind == 'hidden') return ChildWebviewHidden(id);
      if (kind == 'message') {
        final source = arguments['source'];
        final message = arguments['message'];
        if (source is! String || message is! String) return null;
        return ChildWebviewMessage(id, source, message);
      }
      if (kind == 'frameMessage') {
        final source = arguments['source'];
        final message = arguments['message'];
        if (source is! String || message is! String) return null;
        return ChildWebviewFrameMessage(id, source, message);
      }
      if (kind == 'navigationBlocked') {
        final what = arguments['what'];
        final origin = arguments['origin'];
        final external = arguments['external'];
        if (what is! String) return null;
        return ChildWebviewNavigationBlocked(id, what,
            origin is String ? origin : '', external == true);
      }
    } else if (method == 'floatingChatEvent') {
      if (kind == 'closed') return FloatingChatClosed(id);
      if (kind == 'blocked') {
        final what = arguments['what'];
        if (what is! String) return null;
        final origin = arguments['origin'];
        return FloatingChatBlocked(id, what, origin is String ? origin : '');
      }
    }
    return null;
  }

  /// Creates the child window [id] (hidden), maps [hostName] to [folder] and
  /// opens [url]. Throws a [PlatformException] when [id] already exists or
  /// the creation fails; call [dispose] after a failure.
  static Future<void> create({
    required int id,
    required String url,
    required String hostName,
    required String folder,
  }) =>
      _channel.invokeMethod<void>('childWebviewCreate', <String, Object>{
        'id': id,
        'url': url,
        'hostName': hostName,
        'folder': folder,
      });

  /// Shows (and focuses) or hides the page [id].
  static Future<void> setVisible(int id, bool visible) => _channel
      .invokeMethod<void>('childWebviewSetVisible', <String, Object>{
        'id': id,
        'visible': visible,
      });

  /// Sends [message] to the page [id] (its `window.chrome.webview` gets a
  /// `message` event).
  static Future<void> postMessage(int id, String message) =>
      _channel.invokeMethod<void>('childWebviewPostMessage',
          <String, Object>{'id': id, 'message': message});

  /// Sends [json] (a JSON text) to the page [id]; its `message` event gets
  /// the parsed value (design-web-ui 6).
  static Future<void> postJson(int id, String json) => _channel.invokeMethod<
      void>('childWebviewPostJson', <String, Object>{'id': id, 'json': json});

  /// Runs [script] in every YouTube live chat iframe of the page [id] when its
  /// document has loaded, and at once in those already loaded (instruction
  /// 444). What the iframe posts with `window.chrome.webview.postMessage`
  /// arrives as [ChildWebviewFrameMessage]. An empty [script] stops it for
  /// new documents.
  static Future<void> setFrameScript(int id, String script) =>
      _channel.invokeMethod<void>('childWebviewSetFrameScript',
          <String, Object>{'id': id, 'script': script});

  /// Closes the child window [id]. Safe to call when there is none.
  static Future<void> dispose(int id) => _channel
      .invokeMethod<void>('childWebviewDispose', <String, Object>{'id': id});
}

/// Pyokomin (ADR-0067, #298): YouTube's real chat in a floating window (an
/// unowned top-level window with its own taskbar button), addressed by an id
/// per stream.
///
/// The chat area of the app embeds live_chat with `embed_domain`, where
/// YouTube leaves out delete and the other moderation items; this window
/// opens live_chat directly. It only navigates inside live_chat, opens no new
/// windows and takes no drops. Where it goes and whether it stays open is up
/// to the user. Events arrive on [ChildWebview.events].
class FloatingChat {
  FloatingChat._();

  /// Opens [url] (must be a live_chat URL) in the floating chat [id], or
  /// brings the open one to the front (it is not created twice).
  static Future<void> open(int id, String url) => _channel.invokeMethod<void>(
      'floatingChatOpen', <String, Object>{'id': id, 'url': url});

  /// Closes the floating chat [id]. Safe to call when there is none.
  static Future<void> close(int id) => _channel
      .invokeMethod<void>('floatingChatClose', <String, Object>{'id': id});
}
