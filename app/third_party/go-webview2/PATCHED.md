Copy of github.com/jchv/go-webview2 v0.0.0-20260205173254-56598839c808 (MIT, see LICENSE) with one change, in pkg/edge: the COM wrappers
returned an error whenever GetLastError() was not 0 after a call, which is unrelated to the call's result
(a stale "Overlapped I/O operation is in progress" made the window creation fail at random). Those checks
are disabled ("&& false"). cmd/ (demo) removed.
