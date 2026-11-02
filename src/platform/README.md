# src/platform/

The seam between HomRec's engine and the operating system.

- `*.h` here - **interfaces** (what the engine needs: capture, audio, windows, hotkeys, child
  processes, paths). OS-neutral C++17, no `<windows.h>`, no X11.
- `linux/` - the Linux side. Today: session detection, XDG paths, "not implemented yet" stubs for
  every backend, and the `homrec` command-line entry point. `linux/x11/` and `linux/wayland/`
  are where the two real capture backends go (organised by display server, not by distro).
- There is no `windows/` folder yet on purpose: the existing Windows code stays in `src/core/` until
  the capture seam (phase 5 of the plan) moves it behind these interfaces.

Plan, status and the Win32 -> Linux mapping: [`docs/LINUX_PORT_PLAN.md`](../../docs/LINUX_PORT_PLAN.md).
