// scripts/tests/test_x11_services.cpp - the X11 window list and global hotkeys
// (src/platform/linux/x11/x11_windows.cpp, x11_hotkeys.cpp).
//
// Build: scripts/linux/test.sh (links -lX11 -ldl). The cases that need a display start their own
// Xvfb on :96 and are skipped when Xvfb or libXtst.so.6 is missing. Key presses are real X events
// made with XTest (libXtst is loaded with dlopen, so no libXtst header is needed to build).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "platform/linux/x11/x11_hotkeys.h"
#include "platform/linux/x11/x11_windows.h"
#include "platform/process_backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace hr::platform;

namespace {

bool OnPath(const char *name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// One Xvfb for the whole run, killed when the program ends.
struct Server {
    std::unique_ptr<IChildProcess> proc;
    bool ok = false;
    Server() {
        if (!OnPath("Xvfb")) return;
        const std::string sock = "/tmp/.X11-unix/X96";
        struct stat st;
        if (stat(sock.c_str(), &st) == 0) {
            if (Display *d = XOpenDisplay(":96")) { XCloseDisplay(d); return; }   // :96 belongs to somebody else
            unlink(sock.c_str());                                                  // left behind by a killed Xvfb
            unlink("/tmp/.X96-lock");
        }
        proc = CreateChildProcess();
        SpawnSpec xs;
        xs.exe  = "Xvfb";
        xs.args = {":96", "-screen", "0", "800x600x24", "-nolisten", "tcp"};
        if (!proc->Start(xs, nullptr)) return;
        for (int i = 0; i < 100 && stat(sock.c_str(), &st) != 0; ++i) usleep(50 * 1000);
        ok = stat(sock.c_str(), &st) == 0;
        if (ok) setenv("DISPLAY", ":96", 1);
    }
    ~Server() {
        if (!proc) return;
        proc->RequestStop();   // lets Xvfb remove its socket and lock file
        if (!proc->Wait(3000)) proc->Kill();
    }
};

Server &Xvfb() {
    static Server s;
    return s;
}

#define NEED_XVFB() do { if (!Xvfb().ok) { MESSAGE("Xvfb missing or :96 in use - skipped"); return; } } while (0)

// A second program that owns a pid different from ours.
struct Sleeper {
    pid_t pid = -1;
    Sleeper() {
        pid = fork();
        if (pid == 0) { execlp("sleep", "sleep", "60", (char *)nullptr); _exit(127); }
    }
    ~Sleeper() {
        if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
    }
};

// The test's own connection: creates windows and sets their EWMH properties.
struct Client {
    Display *d = XOpenDisplay(nullptr);
    ~Client() { if (d) XCloseDisplay(d); }

    Window Make(const std::string &title, long pid, int x, int y, int w, int h, const char *cls = nullptr) {
        Window win = XCreateSimpleWindow(d, DefaultRootWindow(d), x, y, w, h, 0, 0, 0);
        if (!title.empty())
            XChangeProperty(d, win, XInternAtom(d, "_NET_WM_NAME", False), XInternAtom(d, "UTF8_STRING", False), 8,
                            PropModeReplace, (const unsigned char *)title.data(), (int)title.size());
        if (pid > 0) {
            long p = pid;
            XChangeProperty(d, win, XInternAtom(d, "_NET_WM_PID", False), XA_CARDINAL, 32, PropModeReplace,
                            (const unsigned char *)&p, 1);
        }
        if (cls) {
            char *c = const_cast<char *>(cls);
            XClassHint ch = {c, c};
            XSetClassHint(d, win, &ch);
        }
        XMapWindow(d, win);
        XSync(d, False);
        return win;
    }
    void SetLongs(Window w, const char *prop, Atom type, std::vector<long> v) {
        XChangeProperty(d, w, XInternAtom(d, prop, False), type, 32, PropModeReplace,
                        (const unsigned char *)v.data(), (int)v.size());
        XSync(d, False);
    }
    void ClearRoot(const char *prop) {
        XDeleteProperty(d, DefaultRootWindow(d), XInternAtom(d, prop, False));
        XSync(d, False);
    }
};

}  // namespace

// ---- key strings (no display needed) -------------------------------------------------------------

TEST_CASE("ParseHotkeyString accepts what the Windows parser accepts") {
    unsigned m = 99;
    unsigned long k = 0;
    std::string err;
    CHECK(ParseHotkeyString("F9", &m, &k, &err));
    CHECK(m == 0);
    CHECK(k == XK_F9);
    CHECK(ParseHotkeyString("Ctrl+Shift+R", &m, &k, &err));
    CHECK(m == (ControlMask | ShiftMask));
    CHECK(k == XK_r);
    CHECK(ParseHotkeyString("alt+f10", &m, &k, &err));
    CHECK(m == Mod1Mask);
    CHECK(k == XK_F10);
    CHECK(ParseHotkeyString("Win+1", &m, &k, &err));
    CHECK(m == Mod4Mask);
    CHECK(k == XK_1);
    CHECK(ParseHotkeyString("Super+Control+PageUp", &m, &k, &err));
    CHECK(m == (Mod4Mask | ControlMask));
    CHECK(k == XK_Prior);
    CHECK(ParseHotkeyString("Space", &m, &k, &err));
    CHECK(k == XK_space);
    CHECK(ParseHotkeyString("F24", &m, &k, &err));
}

TEST_CASE("ParseHotkeyString rejects bad strings and says why") {
    std::string err;
    for (const char *bad : {"", "Ctrl+", "Ctrl+Foo", "Hyper+R", "F25", "F0", "Ctrl+Shift", "ab", "+R"}) {
        err.clear();
        CHECK_FALSE(ParseHotkeyString(bad, nullptr, nullptr, &err));
        CHECK_FALSE(err.empty());
    }
    CHECK_FALSE(ParseHotkeyString("Hyper+R", nullptr, nullptr, nullptr));   // err may be null
}

// ---- windows -------------------------------------------------------------------------------------

TEST_CASE("no display: the factories say so") {
    const char *old = std::getenv("DISPLAY");
    const std::string saved = old ? old : "";
    setenv("DISPLAY", "", 1);
    std::string why;
    CHECK(CreateX11WindowService(&why) == nullptr);
    CHECK_FALSE(why.empty());
    why.clear();
    CHECK_FALSE(X11HotkeysAvailable(&why));
    CHECK_FALSE(why.empty());
    if (old) setenv("DISPLAY", saved.c_str(), 1); else unsetenv("DISPLAY");
}

TEST_CASE("window list without a window manager: fields, own windows and untitled ones left out") {
    NEED_XVFB();
    Sleeper other;
    Client c;
    REQUIRE(c.d);
    const Window a    = c.Make("alpha \xc3\xbc", other.pid, 10, 20, 200, 100, "Bar");
    const Window self = c.Make("self", getpid(), 0, 0, 50, 50);
    const Window none = c.Make("", other.pid, 0, 0, 50, 50);
    (void)self; (void)none;

    std::string why;
    auto svc = CreateX11WindowService(&why);
    REQUIRE_MESSAGE(svc, why);
    const auto v = svc->EnumerateWindows();
    REQUIRE(v.size() == 1);
    const WindowInfo &w = v[0];
    CHECK(w.id == a);
    CHECK(w.title == "alpha \xc3\xbc");
    CHECK(w.pid == (uint64_t)other.pid);
    CHECK(w.exe == "sleep");
    CHECK(w.wm_class == "Bar");
    CHECK(w.x == 10);
    CHECK(w.y == 20);
    CHECK(w.w == 200);
    CHECK(w.h == 100);
    CHECK(w.visible);
    CHECK_FALSE(w.minimized);
}

TEST_CASE("window list with EWMH: order, frame extents, minimized") {
    NEED_XVFB();
    Sleeper other;
    Client c;
    REQUIRE(c.d);
    const Window a = c.Make("alpha", other.pid, 10, 20, 200, 100);
    const Window b = c.Make("beta", other.pid, 300, 40, 100, 100);
    const Window root = DefaultRootWindow(c.d);
    auto svc = CreateX11WindowService(nullptr);
    REQUIRE(svc);

    // The window manager says: alpha at the bottom, beta on top.
    c.SetLongs(root, "_NET_CLIENT_LIST_STACKING", XA_WINDOW, {(long)a, (long)b});
    auto v = svc->EnumerateWindows();
    REQUIRE(v.size() == 2);
    CHECK(v[0].title == "beta");
    CHECK(v[1].title == "alpha");

    // Only the windows the manager lists count; its order wins over the X stacking order.
    c.SetLongs(root, "_NET_CLIENT_LIST_STACKING", XA_WINDOW, {(long)b, (long)a});
    v = svc->EnumerateWindows();
    REQUIRE(v.size() == 2);
    CHECK(v[0].title == "alpha");
    c.SetLongs(root, "_NET_CLIENT_LIST_STACKING", XA_WINDOW, {(long)a});
    v = svc->EnumerateWindows();
    REQUIRE(v.size() == 1);
    CHECK(v[0].title == "alpha");

    // Frame = client area + extents (left, right, top, bottom).
    c.SetLongs(a, "_NET_FRAME_EXTENTS", XA_CARDINAL, {2, 3, 20, 4});
    v = svc->EnumerateWindows();
    REQUIRE(v.size() == 1);
    CHECK(v[0].x == 8);
    CHECK(v[0].y == 0);
    CHECK(v[0].w == 205);
    CHECK(v[0].h == 124);

    // Only _NET_CLIENT_LIST (no stacking list): used as it is.
    c.ClearRoot("_NET_CLIENT_LIST_STACKING");
    c.SetLongs(root, "_NET_CLIENT_LIST", XA_WINDOW, {(long)b});
    v = svc->EnumerateWindows();
    REQUIRE(v.size() == 1);
    CHECK(v[0].title == "beta");

    // Minimized (hidden) windows are listed, flagged, and not "visible".
    c.SetLongs(b, "_NET_WM_STATE", XA_ATOM, {(long)XInternAtom(c.d, "_NET_WM_STATE_HIDDEN", False)});
    v = svc->EnumerateWindows();
    REQUIRE(v.size() == 1);
    CHECK(v[0].minimized);
    CHECK_FALSE(v[0].visible);

    c.ClearRoot("_NET_CLIENT_LIST");
}

TEST_CASE("Query and IsAlive: a destroyed or invented window is 'gone', not a crash") {
    NEED_XVFB();
    Sleeper other;
    Client c;
    REQUIRE(c.d);
    const Window a = c.Make("alpha", other.pid, 10, 20, 200, 100);
    auto svc = CreateX11WindowService(nullptr);
    REQUIRE(svc);

    WindowInfo wi;
    CHECK(svc->IsAlive(a));
    CHECK(svc->Query(a, &wi));
    CHECK(wi.title == "alpha");

    XDestroyWindow(c.d, a);
    XSync(c.d, False);
    CHECK_FALSE(svc->IsAlive(a));
    CHECK_FALSE(svc->Query(a, &wi));
    CHECK_FALSE(svc->IsAlive(0));
    CHECK_FALSE(svc->Query(0x7fffff00, &wi));
    CHECK(svc->EnumerateWindows().empty());   // the service still works after the errors
}

// ---- hotkeys -------------------------------------------------------------------------------------

namespace {

typedef int (*FakeKeyFn)(Display *, unsigned int, int, unsigned long);

struct Keys {
    Client c;
    void *lib = dlopen("libXtst.so.6", RTLD_NOW);
    FakeKeyFn fake = lib ? (FakeKeyFn)dlsym(lib, "XTestFakeKeyEvent") : nullptr;
    bool Ok() const { return c.d && fake; }
    unsigned Code(KeySym s) { return XKeysymToKeycode(c.d, s); }
    void Down(KeySym s) { fake(c.d, Code(s), True, 0); XSync(c.d, False); }
    void Up(KeySym s) { fake(c.d, Code(s), False, 0); XSync(c.d, False); }
    void Tap(KeySym s) { Down(s); Up(s); }
    // No dlclose: libXtst hooks into the display, and XCloseDisplay (member `c`) runs after this body.
};

struct Got {
    std::mutex m;
    std::condition_variable cv;
    std::vector<int> ids;
    static void Cb(void *u, int id) {
        Got *g = static_cast<Got *>(u);
        std::lock_guard<std::mutex> l(g->m);
        g->ids.push_back(id);
        g->cv.notify_all();
    }
    // Waits until `n` ids arrived (true) or `ms` passed.
    bool Wait(size_t n, int ms) {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return ids.size() >= n; });
    }
    size_t Count() { std::lock_guard<std::mutex> l(m); return ids.size(); }
};

}  // namespace

TEST_CASE("hotkeys: real key events reach the right binding, Stop and restart work") {
    NEED_XVFB();
    Keys k;
    if (!k.Ok()) { MESSAGE("libXtst.so.6 missing - skipped"); return; }

    std::string err;
    auto hk = CreateX11HotkeyService(&err);
    REQUIRE_MESSAGE(hk, err);
    Got got;
    hk->SetCallback(&Got::Cb, &got);
    REQUIRE_MESSAGE(hk->Register(1, "F9", &err), err);
    REQUIRE_MESSAGE(hk->Register(2, "Ctrl+Shift+R", &err), err);
    REQUIRE(hk->Start());

    k.Tap(XK_F9);
    REQUIRE(got.Wait(1, 2000));
    CHECK(got.ids[0] == 1);

    k.Down(XK_Control_L);
    k.Down(XK_Shift_L);
    k.Tap(XK_r);
    k.Up(XK_Shift_L);
    k.Up(XK_Control_L);
    REQUIRE(got.Wait(2, 2000));
    CHECK(got.ids[1] == 2);

    // Not bound: plain R, Ctrl+R, Shift+F9 (extra modifier).
    k.Tap(XK_r);
    k.Down(XK_Control_L); k.Tap(XK_r); k.Up(XK_Control_L);
    k.Down(XK_Shift_L); k.Tap(XK_F9); k.Up(XK_Shift_L);
    CHECK_FALSE(got.Wait(3, 400));

    // Num Lock on (when the keymap has it): the binding still fires.
    if (k.Code(XK_Num_Lock)) {
        k.Tap(XK_Num_Lock);
        k.Tap(XK_F9);
        CHECK(got.Wait(3, 2000));
        k.Tap(XK_Num_Lock);
    }

    // Stop: no more callbacks; a second Stop is harmless; Start again works.
    hk->Stop();
    hk->Stop();
    const size_t before = got.Count();
    k.Tap(XK_F9);
    usleep(300 * 1000);
    CHECK(got.Count() == before);
    REQUIRE(hk->Start());
    k.Tap(XK_F9);
    CHECK(got.Wait(before + 1, 2000));
    hk->Stop();
}

TEST_CASE("hotkeys: a held key fires once") {
    NEED_XVFB();
    Keys k;
    if (!k.Ok()) { MESSAGE("libXtst.so.6 missing - skipped"); return; }
    auto hk = CreateX11HotkeyService(nullptr);
    REQUIRE(hk);
    Got got;
    hk->SetCallback(&Got::Cb, &got);
    REQUIRE(hk->Register(1, "F8", nullptr));
    REQUIRE(hk->Start());
    k.Down(XK_F8);
    usleep(1500 * 1000);   // longer than the X server's auto-repeat delay
    k.Up(XK_F8);
    CHECK(got.Wait(1, 2000));
    usleep(200 * 1000);
    CHECK(got.Count() == 1);
    hk->Stop();
}

TEST_CASE("hotkeys: bad strings, own duplicates, a key another program owns, release on destroy") {
    NEED_XVFB();
    std::string err;
    auto a = CreateX11HotkeyService(&err);
    REQUIRE_MESSAGE(a, err);
    CHECK_FALSE(a->Register(1, "Hyper+R", &err));
    CHECK(err.find("modifier") != std::string::npos);

    REQUIRE(a->Register(1, "F7", &err));
    CHECK_FALSE(a->Register(2, "f7", &err));
    CHECK(err.find("another HomRec hotkey") != std::string::npos);
    REQUIRE(a->Register(1, "F6", &err));   // same id again = rebind: F7 is free now
    REQUIRE(a->Register(2, "F7", &err));

    auto b = CreateX11HotkeyService(&err);   // a different connection = "another program"
    REQUIRE(b);
    CHECK_FALSE(b->Register(5, "F7", &err));
    CHECK(err.find("another running application") != std::string::npos);
    CHECK(b->Register(5, "F5", &err));       // the failed grab left nothing behind

    a->Unregister(2);
    CHECK(b->Register(6, "F7", &err));       // free again after Unregister
    a.reset();
    CHECK(b->Register(7, "F6", &err));       // and after the service is destroyed
}
