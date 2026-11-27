// x11_error_trap.h - lets a block of Xlib calls fail (BadWindow, BadAccess) without Xlib's default
// handler killing the process. Xlib has ONE error handler per process, so a trap is only safe when
// no other thread is making Xlib calls at the same time (the services in this folder are used from
// one thread at a time, see their headers).
#pragma once

#include <X11/Xlib.h>

namespace hr {
namespace platform {

class X11ErrorTrap {
public:
    explicit X11ErrorTrap(Display *d) : d_(d) {
        XSync(d_, False);
        code_ = 0;
        old_  = XSetErrorHandler(&Handler);
    }
    ~X11ErrorTrap() {
        XSync(d_, False);
        XSetErrorHandler(old_);
    }
    X11ErrorTrap(const X11ErrorTrap &) = delete;
    X11ErrorTrap &operator=(const X11ErrorTrap &) = delete;

    // 0 = no X error since the trap was created; otherwise the error code (BadWindow, BadAccess, ...).
    int Code() {
        XSync(d_, False);
        return code_;
    }

private:
    static int Handler(Display *, XErrorEvent *e) {
        code_ = e->error_code;
        return 0;
    }
    static inline int code_ = 0;
    Display *d_;
    int (*old_)(Display *, XErrorEvent *);
};

}  // namespace platform
}  // namespace hr
