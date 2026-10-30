import 'dart:async';

import 'package:flutter/services.dart';

const MethodChannel _channel = MethodChannel('io.jns.webview.win');
const MethodChannel _eventChannel = MethodChannel('io.jns.webview.win/child');

/// Pyokomin (ADR-0063, ADR-0064, ADR-0065, #298): what a child-window webview
/// is for.
enum ChildWebviewRole {
  /// The app's page. Gets a virtual host mapped to a local folder.
  page,

  /// A stream's chat. Opens YouTube's live_chat directly; navigations outside
  /// live_chat and new windows are cancelled (reported as
  /// [ChildWebviewBlocked]), drops are refused. Shown in the chat area with
  /// [ChildWebview.showChat].
  chat,
}

/// An event from the child-window webviews.
abstract class ChildWebviewEvent {
  const ChildWebviewEvent();
}

/// The page [id] hid itself (`postMessage('hide')`).
class ChildWebviewHidden extends ChildWebviewEvent {
  const ChildWebviewHidden(this.id);
  final int id;
}

/// A web message from [id], from the page's own origin or (chat) from
/// `https://www.youtube.com/`. [source] is the sender's URL.
class ChildWebviewMessage extends ChildWebviewEvent {
  const ChildWebviewMessage(this.id, this.source, this.message);
  final int id;
  final String source;
  final String message;
}

/// A navigation (`navigation`) or a new window (`newWindow`) of [id] was
/// cancelled. The URL is not passed on.
class ChildWebviewBlocked extends ChildWebviewEvent {
  const ChildWebviewBlocked(this.id, this.what);
  final int id;
  final String what;
}

/// The user dragged the splitter. [width] is the new width of the chat area
/// in logical pixels (sent once, when the drag ends).
class ChildWebviewChatWidthChanged extends ChildWebviewEvent {
  const ChildWebviewChatWidthChanged(this.width);
  final int width;
}

/// Pyokomin (ADR-0063, ADR-0065, #298): webviews hosted in real child windows
/// of the app window (plain WebView2 controllers, not the texture-based
/// ones), addressed by an id.
///
/// The native side lays them out side by side, from the left:
/// the page | a splitter | the chat area (one chat shown at a time). They
/// never overlap. They share the environment (profile and sign-in) with the
/// other webviews. When the app window becomes active, the focus goes back to
/// the child that had it last.
class ChildWebview {
  ChildWebview._();

  static StreamController<ChildWebviewEvent>? _events;

  /// Events of all child windows.
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
    if (method == 'childWebviewChatWidthChanged') {
      final width = arguments['width'];
      return width is int ? ChildWebviewChatWidthChanged(width) : null;
    }
    if (method != 'childWebviewEvent') return null;
    final id = arguments['id'];
    if (id is! int) return null;
    switch (arguments['kind']) {
      case 'hidden':
        return ChildWebviewHidden(id);
      case 'message':
        final source = arguments['source'];
        final message = arguments['message'];
        if (source is! String || message is! String) return null;
        return ChildWebviewMessage(id, source, message);
      case 'blocked':
        final what = arguments['what'];
        if (what is! String) return null;
        return ChildWebviewBlocked(id, what);
    }
    return null;
  }

  /// Creates the child window [id] (hidden) and opens [url]. A [page] needs
  /// [hostName] and [folder] (the virtual host mapping). Throws a
  /// [PlatformException] when [id] already exists or the creation fails;
  /// call [dispose] after a failure.
  static Future<void> create({
    required int id,
    required ChildWebviewRole role,
    required String url,
    String? hostName,
    String? folder,
  }) =>
      _channel.invokeMethod<void>('childWebviewCreate', <String, Object>{
        'id': id,
        'role': role.name,
        'url': url,
        if (hostName != null) 'hostName': hostName,
        if (folder != null) 'folder': folder,
      });

  /// Shows (and focuses) or hides the page [id]. The chat area is shown only
  /// while the page is.
  static Future<void> setVisible(int id, bool visible) => _channel
      .invokeMethod<void>('childWebviewSetVisible', <String, Object>{
        'id': id,
        'visible': visible,
      });

  /// Shows the chat [id] in the chat area (the other chats are hidden but
  /// kept alive). `null` closes the chat area; the page fills the window.
  static Future<void> showChat(int? id) => _channel
      .invokeMethod<void>('childWebviewShowChat', <String, Object?>{'id': id});

  /// The width of the chat area in logical pixels (from the settings).
  static Future<void> setChatWidth(int width) => _channel.invokeMethod<void>(
      'childWebviewSetChatWidth', <String, Object>{'width': width});

  /// The color of the strip above the chat (0xAARRGGBB, alpha ignored).
  static Future<void> setChatAccent(int color) => _channel.invokeMethod<void>(
      'childWebviewSetChatAccent', <String, Object>{'color': color});

  /// Closes the child window [id]. Closing the shown chat closes the chat
  /// area. Safe to call when there is none.
  static Future<void> dispose(int id) => _channel
      .invokeMethod<void>('childWebviewDispose', <String, Object>{'id': id});
}
