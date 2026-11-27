// x11_windows.cpp - IWindowService on X11.
//
// The window list is the window manager's own: _NET_CLIENT_LIST_STACKING (bottom to top, reversed
// here), else _NET_CLIENT_LIST. A session without an EWMH window manager has neither; then the
// children of the root window are used instead (override-redirect windows skipped).
// Title, pid and frame come from _NET_WM_NAME, _NET_WM_PID and _NET_FRAME_EXTENTS.
#include "x11_windows.h"
#include "x11_error_trap.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace hr {
namespace platform {

namespace {

struct Atoms {
    Atom client_list, client_stacking, wm_name, utf8, wm_pid, wm_state, hidden, frame_extents;
};

// Format-32 property as longs (Xlib hands those back as `long`). Empty when missing or wrong type.
std::vector<long> GetLongs(Display *d, Window w, Atom prop, Atom type, long max_items) {
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = nullptr;
    std::vector<long> out;
    if (XGetWindowProperty(d, w, prop, 0, max_items, False, type, &actual, &fmt, &n, &after, &data) == Success &&
        data) {
        if (fmt == 32 && actual == type) out.assign(reinterpret_cast<long *>(data), reinterpret_cast<long *>(data) + n);
        XFree(data);
    }
    return out;
}

std::string GetUtf8(Display *d, Window w, Atom prop, Atom utf8) {
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = nullptr;
    std::string out;
    if (XGetWindowProperty(d, w, prop, 0, 4096, False, utf8, &actual, &fmt, &n, &after, &data) == Success && data) {
        if (fmt == 8 && actual == utf8) out.assign(reinterpret_cast<char *>(data), n);
        XFree(data);
    }
    return out;
}

std::string ReadLink(const std::string &p) {
    char buf[4096];
    const ssize_t n = readlink(p.c_str(), buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

std::string ReadFirstLine(const std::string &p) {
    std::string s;
    if (FILE *f = std::fopen(p.c_str(), "rb")) {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), f)) s = buf;
        std::fclose(f);
    }
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Base name of the program: /proc/<pid>/exe when readable (another user's process is not), else
// /proc/<pid>/comm (cut to 15 characters by the kernel).
std::string ExeName(uint64_t pid) {
    if (!pid) return {};
    const std::string base = "/proc/" + std::to_string((unsigned long long)pid) + "/";
    std::string exe = ReadLink(base + "exe");
    const std::string del = " (deleted)";
    if (exe.size() > del.size() && exe.compare(exe.size() - del.size(), del.size(), del) == 0)
        exe.erase(exe.size() - del.size());
    if (exe.empty()) return ReadFirstLine(base + "comm");
    const size_t slash = exe.rfind('/');
    return slash == std::string::npos ? exe : exe.substr(slash + 1);
}

class X11WindowService : public IWindowService {
public:
    explicit X11WindowService(Display *d) : d_(d), root_(DefaultRootWindow(d)) {
        a_.client_list     = XInternAtom(d_, "_NET_CLIENT_LIST", False);
        a_.client_stacking = XInternAtom(d_, "_NET_CLIENT_LIST_STACKING", False);
        a_.wm_name         = XInternAtom(d_, "_NET_WM_NAME", False);
        a_.utf8            = XInternAtom(d_, "UTF8_STRING", False);
        a_.wm_pid          = XInternAtom(d_, "_NET_WM_PID", False);
        a_.wm_state        = XInternAtom(d_, "_NET_WM_STATE", False);
        a_.hidden          = XInternAtom(d_, "_NET_WM_STATE_HIDDEN", False);
        a_.frame_extents   = XInternAtom(d_, "_NET_FRAME_EXTENTS", False);
    }
    ~X11WindowService() override { XCloseDisplay(d_); }

    std::vector<WindowInfo> EnumerateWindows() override {
        std::lock_guard<std::mutex> lock(mu_);
        X11ErrorTrap trap(d_);
        std::vector<Window> ids = ClientList();
        std::vector<WindowInfo> out;
        const uint64_t self = (uint64_t)getpid();
        for (Window w : ids) {
            WindowInfo wi;
            if (!Fill(w, &wi) || wi.title.empty() || (wi.pid && wi.pid == self)) continue;
            out.push_back(wi);
        }
        return out;
    }

    bool Query(uint64_t id, WindowInfo *out) override {
        std::lock_guard<std::mutex> lock(mu_);
        X11ErrorTrap trap(d_);
        WindowInfo wi;
        if (!Fill((Window)id, &wi) || trap.Code() != 0) return false;
        if (out) *out = wi;
        return true;
    }

    bool IsAlive(uint64_t id) override {
        std::lock_guard<std::mutex> lock(mu_);
        X11ErrorTrap trap(d_);
        XWindowAttributes attr;
        return XGetWindowAttributes(d_, (Window)id, &attr) != 0 && trap.Code() == 0;
    }

private:
    // Top of the z-order first.
    std::vector<Window> ClientList() {
        std::vector<Window> ids;
        std::vector<long> v = GetLongs(d_, root_, a_.client_stacking, XA_WINDOW, 65536);
        if (!v.empty()) {
            std::reverse(v.begin(), v.end());
        } else {
            v = GetLongs(d_, root_, a_.client_list, XA_WINDOW, 65536);
        }
        for (long x : v) ids.push_back((Window)x);
        if (!ids.empty()) return ids;

        // No EWMH window manager: the root's children, topmost first.
        Window r, p, *kids = nullptr;
        unsigned n = 0;
        if (XQueryTree(d_, root_, &r, &p, &kids, &n) && kids) {
            for (unsigned i = n; i-- > 0;) {
                XWindowAttributes attr;
                if (XGetWindowAttributes(d_, kids[i], &attr) && !attr.override_redirect) ids.push_back(kids[i]);
            }
            XFree(kids);
        }
        return ids;
    }

    bool Fill(Window w, WindowInfo *wi) {
        XWindowAttributes attr;
        if (!XGetWindowAttributes(d_, w, &attr)) return false;
        wi->id = (uint64_t)w;

        wi->title = GetUtf8(d_, w, a_.wm_name, a_.utf8);
        if (wi->title.empty()) {
            char *name = nullptr;
            if (XFetchName(d_, w, &name) && name) {
                wi->title = name;
                XFree(name);
            }
        }

        const std::vector<long> pid = GetLongs(d_, w, a_.wm_pid, XA_CARDINAL, 1);
        wi->pid = pid.empty() ? 0 : (uint64_t)pid[0];
        wi->exe = ExeName(wi->pid);

        XClassHint ch = {nullptr, nullptr};
        if (XGetClassHint(d_, w, &ch)) {
            if (ch.res_class) wi->wm_class = ch.res_class;
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        }

        const std::vector<long> state = GetLongs(d_, w, a_.wm_state, XA_ATOM, 64);
        wi->minimized = std::find(state.begin(), state.end(), (long)a_.hidden) != state.end();
        wi->visible   = attr.map_state == IsViewable && !wi->minimized;

        // Client area -> frame rectangle in root coordinates.
        Window child;
        int x = 0, y = 0;
        XTranslateCoordinates(d_, w, root_, 0, 0, &x, &y, &child);
        int fw = attr.width, fh = attr.height;
        const std::vector<long> ext = GetLongs(d_, w, a_.frame_extents, XA_CARDINAL, 4);   // left, right, top, bottom
        if (ext.size() == 4) {
            x -= (int)ext[0];
            y -= (int)ext[2];
            fw += (int)(ext[0] + ext[1]);
            fh += (int)(ext[2] + ext[3]);
        }
        wi->x = x;
        wi->y = y;
        wi->w = fw;
        wi->h = fh;
        return true;
    }

    Display *d_;
    Window   root_;
    Atoms    a_;
    std::mutex mu_;
};

}  // namespace

std::unique_ptr<IWindowService> CreateX11WindowService(std::string *why_not) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        if (why_not) {
            const char *env = std::getenv("DISPLAY");
            *why_not = std::string("cannot open the X display '") + (env ? env : "") + "'";
        }
        return nullptr;
    }
    return std::unique_ptr<IWindowService>(new X11WindowService(d));
}

}  // namespace platform
}  // namespace hr
