# src/platform/linux/wayland/

Empty on purpose - see `../x11/README.md` for the full explanation of why
Linux capture code is organized by display server (X11 vs Wayland)
instead of by distro (Arch vs Debian).

When this gets implemented, it'll most likely be PipeWire + the
xdg-desktop-portal ScreenCast portal (org.freedesktop.portal.ScreenCast) —
that's the standard, compositor-agnostic way to capture a screen/window
under Wayland, and works the same on GNOME, KDE, etc. without needing a
separate backend per compositor.
