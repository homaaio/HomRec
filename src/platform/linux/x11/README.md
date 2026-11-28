# src/platform/linux/x11/

What is here: `x11_record` (recording through ffmpeg `x11grab`), `x11_windows` (EWMH window list,
`IWindowService`) and `x11_hotkeys` (XGrabKey, `IHotkeyService`) - phase 2 of
[`plan-linux.md`](../../../../plan-linux.md). Not here yet: the monitor list and a frame-by-frame
`IScreenCapture` (`capture_backend.h`). The factories that pick these files live in
`../backend_stubs.cpp`.

**Why "x11", not "arch"/"deb"/a distro name:** screen capture on Linux is
split by *display server*, not by distribution. An Arch user and a Debian
user running the same display server (X11 or Wayland) need the exact same
capture code — capturing frames from an X11 session works identically
regardless of which package manager put X11 there. So this folder — and
its sibling `../wayland/` — is where the two actual Linux capture
backends will live once they exist:

- `x11/` — capture via Xlib/XShm (or similar), for X11 sessions.
- `wayland/` — capture via PipeWire + the xdg-desktop-portal
  ScreenCast API, for Wayland sessions (X11-style direct capture isn't
  possible under Wayland's security model — this is the actual API
  compositors expose for it).

Distro-specific concerns (`.deb` vs Arch's `PKGBUILD`, etc.) are a
*packaging* question, not a source-code one — see `scripts/packaging/`
at the project root for where that belongs instead.

Nothing currently in `src/` needs to move here — the whole existing
codebase is Windows-only (DXGI/D3D11/Win32), so there's no matching
`src/platform/windows/` yet either; that split only makes sense once
the capture seam (plan, phase 5) puts the Windows code behind the same interfaces.
