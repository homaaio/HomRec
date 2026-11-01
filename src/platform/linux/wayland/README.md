# src/platform/linux/wayland/

Still empty on purpose (phase 3 of `docs/LINUX_PORT_PLAN.md`; implements `src/platform/capture_backend.h`).
See `../x11/README.md` for the full explanation of why
Linux capture code is organized by display server (X11 vs Wayland)
instead of by distro (Arch vs Debian).

When this gets implemented, it'll most likely be PipeWire + the
xdg-desktop-portal ScreenCast portal (org.freedesktop.portal.ScreenCast) —
that's the standard, compositor-agnostic way to capture a screen/window
under Wayland, and works the same on GNOME, KDE, etc. without needing a
separate backend per compositor.

Wayland limits that are part of the interface, not bugs: the portal shows a picker (`CaptureCaps::needs_user_prompt`),
applications cannot list other windows (`CreateWindowService` returns nullptr) and global hotkeys depend on the
GlobalShortcuts portal being available.
