/// Loading state
// Order must match WebviewLoadingState (see webview.h)
enum LoadingState { none, loading, navigationCompleted }

/// Pointer button type
// Order must match WebviewPointerButton (see webview.h)
enum PointerButton { none, primary, secondary, tertiary }

enum WebviewDownloadEventKind {
  downloadStarted,
  downloadCompleted,
  downloadProgress
}

/// Pointer Event kind
// Order must match WebviewPointerEventKind (see webview.h)
enum WebviewPointerEventKind { activate, down, enter, leave, up, update }

/// Permission kind
// Order must match WebviewPermissionKind (see webview.h)
enum WebviewPermissionKind {
  unknown,
  microphone,
  camera,
  geoLocation,
  notifications,
  otherSensors,
  clipboardRead
}

enum WebviewPermissionDecision { none, allow, deny }

/// The policy for popup requests.
///
/// [allow] allows popups and will create new windows.
/// [deny] suppresses popups.
/// [sameWindow] displays popup contents in the current WebView.
enum WebviewPopupWindowPolicy { allow, deny, sameWindow }

/// The kind of cross origin resource access for virtual hosts
///
/// [deny] all cross origin requests are denied.
/// [allow] all cross origin requests are allowed.
/// [denyCors] sub resource cross origin requests are allowed, otherwise denied.
///
/// For more detailed information, please refer to
/// [Microsofts](https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2#corewebview2_host_resource_access_kind)
/// documentation.
// Order must match WebviewHostResourceAccessKind (see webview.h)
enum WebviewHostResourceAccessKind { deny, allow, denyCors }

enum WebErrorStatus {
  WebErrorStatusUnknown,
  WebErrorStatusCertificateCommonNameIsIncorrect,
  WebErrorStatusCertificateExpired,
  WebErrorStatusClientCertificateContainsErrors,
  WebErrorStatusCertificateRevoked,
  WebErrorStatusCertificateIsInvalid,
  WebErrorStatusServerUnreachable,
  WebErrorStatusTimeout,
  WebErrorStatusErrorHTTPInvalidServerResponse,
  WebErrorStatusConnectionAborted,
  WebErrorStatusConnectionReset,
  WebErrorStatusDisconnected,
  WebErrorStatusCannotConnect,
  WebErrorStatusHostNameNotResolved,
  WebErrorStatusOperationCanceled,
  WebErrorStatusRedirectFailed,
  WebErrorStatusUnexpectedError,
  WebErrorStatusValidAuthenticationCredentialsRequired,
  WebErrorStatusValidProxyAuthenticationRequired,
}

/// Why a WebView2 helper process died (ISSUES #88 / instruction 162).
///
/// Mirrors COREWEBVIEW2_PROCESS_FAILED_KIND, but only to the depth the caller
/// needs: can the view be brought back with a reload, or does the whole
/// instance have to be recreated?
enum WebviewProcessFailedKind {
  /// The browser process is gone. The instance is unusable; recreate it.
  browserProcessExited,

  /// The renderer died. A reload brings the page back.
  renderProcessExited,

  /// The renderer stopped responding. A reload brings the page back.
  renderProcessUnresponsive,

  /// A helper process such as the GPU process died. A reload is enough.
  helperProcessExited,

  /// Something the mapping does not know about. Treated as "reload" because
  /// that is the cheaper recovery; if it is not enough the caller gives up
  /// after one attempt anyway.
  unknown,
}

/// Maps the native COREWEBVIEW2_PROCESS_FAILED_KIND value.
///
/// The numeric values follow the WebView2 SDK:
///   0 browser process exited
///   1 render process exited
///   2 render process unresponsive
///   3 frame render process exited
///   4 utility process exited
///   5 sandbox helper process exited
///   6 GPU process exited
///   7 PPAPI plugin process exited
///   8 PPAPI broker process exited
///   9 unknown process exited
WebviewProcessFailedKind webviewProcessFailedKindFromCode(int code) {
  switch (code) {
    case 0:
      return WebviewProcessFailedKind.browserProcessExited;
    case 1:
    case 3:
      return WebviewProcessFailedKind.renderProcessExited;
    case 2:
      return WebviewProcessFailedKind.renderProcessUnresponsive;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
      return WebviewProcessFailedKind.helperProcessExited;
    default:
      return WebviewProcessFailedKind.unknown;
  }
}
