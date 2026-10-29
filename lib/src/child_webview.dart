import 'package:flutter/services.dart';

const MethodChannel _channel = MethodChannel('io.jns.webview.win');

/// Pyokomin (ADR-0063, #298): the webview hosted in a real child window of the
/// app window (a plain WebView2 controller, not the texture-based one).
///
/// There is at most one per app. It fills the client area of the app window
/// and shares the environment (profile and sign-in) with the other webviews.
/// The page can hide itself with `window.chrome.webview.postMessage('hide')`
/// (only from `https://<hostName>/`).
class ChildWebview {
  ChildWebview._();

  /// Creates the child window (hidden), maps [hostName] to [folder] and opens
  /// [url]. Throws a [PlatformException] when it already exists or fails;
  /// call [dispose] after a failure.
  static Future<void> create({
    required String url,
    required String hostName,
    required String folder,
  }) =>
      _channel.invokeMethod<void>('childWebviewCreate', <String, Object>{
        'url': url,
        'hostName': hostName,
        'folder': folder,
      });

  /// Shows (and focuses) or hides the child window.
  static Future<void> setVisible(bool visible) =>
      _channel.invokeMethod<void>('childWebviewSetVisible', visible);

  /// Closes the child window. Safe to call when there is none.
  static Future<void> dispose() =>
      _channel.invokeMethod<void>('childWebviewDispose');
}
