// x11_hotkeys.cpp - IHotkeyService on X11: XGrabKey on the root window.
//
// Each binding is grabbed four times (with and without Caps Lock and Num Lock, assumed to be Mod2)
// so the lock keys do not hide it. A combination another program already grabbed answers BadAccess,
// which Register() reports. One worker thread owns the connection after Start(); it waits in poll()
// on the X socket and on a pipe that Stop() writes to.
#include "x11_hotkeys.h"
#include "x11_error_trap.h"

#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

namespace hr {
namespace platform {

namespace {

const unsigned kRelevantMods = ShiftMask | ControlMask | Mod1Mask | Mod4Mask;
const unsigned kLockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

std::string Lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The X11 keysym name for a Windows-style key token, "" when unknown.
std::string KeysymName(const std::string &key) {
    const std::string k = Lower(key);
    if (k.size() >= 2 && k[0] == 'f' && std::isdigit((unsigned char)k[1])) {
        char *end = nullptr;
        const long n = std::strtol(k.c_str() + 1, &end, 10);
        if (*end == '\0' && n >= 1 && n <= 24) return "F" + std::to_string(n);
        return {};
    }
    static const std::map<std::string, std::string> names = {
        {"escape", "Escape"}, {"space", "space"}, {"return", "Return"}, {"enter", "Return"},
        {"tab", "Tab"}, {"backspace", "BackSpace"}, {"delete", "Delete"}, {"insert", "Insert"},
        {"home", "Home"}, {"end", "End"}, {"prior", "Prior"}, {"pageup", "Prior"},
        {"next", "Next"}, {"pagedown", "Next"}, {"up", "Up"}, {"down", "Down"},
        {"left", "Left"}, {"right", "Right"}};
    const auto it = names.find(k);
    if (it != names.end()) return it->second;
    if (k.size() == 1 && std::isalnum((unsigned char)k[0])) return k;
    return {};
}

struct Binding {
    int      id;
    unsigned mods;
    unsigned keycode;
};

class X11HotkeyService : public IHotkeyService {
public:
    explicit X11HotkeyService(Display *d) : d_(d), root_(DefaultRootWindow(d)) {}
    ~X11HotkeyService() override {
        Stop();
        for (const Binding &b : bindings_) Ungrab(b);
        XCloseDisplay(d_);
    }

    bool Register(int id, const std::string &keystring, std::string *err) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) return Fail(err, "hotkeys must be registered before Start()");
        unsigned mods = 0;
        unsigned long sym = 0;
        if (!ParseHotkeyString(keystring, &mods, &sym, err)) return false;
        const KeyCode code = XKeysymToKeycode(d_, (KeySym)sym);
        if (!code) return Fail(err, "'" + keystring + "': the key is not on the current keyboard layout");

        RemoveLocked(id);
        for (const Binding &b : bindings_)
            if (b.keycode == code && b.mods == mods)
                return Fail(err, "'" + keystring + "' is already bound to another HomRec hotkey");

        Binding nb{id, mods, code};
        X11ErrorTrap trap(d_);
        for (unsigned lock_bits : kLockVariants) XGrabKey(d_, code, mods | lock_bits, root_, False, GrabModeAsync, GrabModeAsync);
        if (trap.Code() != 0) {
            Ungrab(nb);
            return Fail(err, "'" + keystring + "' is already used by another running application");
        }
        bindings_.push_back(nb);
        return true;
    }

    void Unregister(int id) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) RemoveLocked(id);
    }

    void SetCallback(HotkeyCallback cb, void *user) override {
        std::lock_guard<std::mutex> lock(mu_);
        cb_   = cb;
        user_ = user;
    }

    bool Start() override {
        std::lock_guard<std::mutex> lock(mu_);
        if (running_) return true;
        if (pipe(wake_) != 0) return false;
        XkbSetDetectableAutoRepeat(d_, True, nullptr);   // a held key = one KeyPress, not a stream
        running_ = true;
        worker_  = std::thread([this] { Run(); });
        return true;
    }

    void Stop() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            const char b = 1;
            if (write(wake_[1], &b, 1) < 0) { /* the worker also ends when the pipe is closed below */ }
        }
        worker_.join();
        std::lock_guard<std::mutex> lock(mu_);
        close(wake_[0]);
        close(wake_[1]);
        running_ = false;
    }

private:
    static bool Fail(std::string *err, const std::string &msg) {
        if (err) *err = msg;
        return false;
    }

    void Ungrab(const Binding &b) {
        X11ErrorTrap trap(d_);
        for (unsigned lock_bits : kLockVariants) XUngrabKey(d_, (int)b.keycode, b.mods | lock_bits, root_);
    }

    void RemoveLocked(int id) {
        for (auto it = bindings_.begin(); it != bindings_.end();) {
            if (it->id == id) {
                Ungrab(*it);
                it = bindings_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Run() {
        const int xfd = ConnectionNumber(d_);
        std::set<unsigned> down;   // keycodes held right now
        for (;;) {
            while (XPending(d_)) {
                XEvent ev;
                XNextEvent(d_, &ev);
                if (ev.type == KeyRelease) {
                    down.erase(ev.xkey.keycode);
                } else if (ev.type == KeyPress) {
                    if (!down.insert(ev.xkey.keycode).second) continue;
                    Fire(ev.xkey.keycode, ev.xkey.state & kRelevantMods);
                }
            }
            pollfd p[2] = {{xfd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (poll(p, 2, -1) < 0) continue;   // EINTR
            if (p[1].revents) break;
        }
    }

    void Fire(unsigned keycode, unsigned mods) {
        HotkeyCallback cb;
        void *user;
        int id = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cb   = cb_;
            user = user_;
            for (const Binding &b : bindings_)
                if (b.keycode == keycode && b.mods == mods) id = b.id;
        }
        if (cb && id >= 0) cb(user, id);
    }

    Display *d_;
    Window   root_;
    std::mutex mu_;
    std::vector<Binding> bindings_;
    HotkeyCallback cb_ = nullptr;
    void *user_ = nullptr;
    bool running_ = false;
    int  wake_[2] = {-1, -1};
    std::thread worker_;
};

}  // namespace

bool ParseHotkeyString(const std::string &s, unsigned *mods, unsigned long *keysym, std::string *err) {
    auto fail = [&](const std::string &m) {
        if (err) *err = "'" + s + "': " + m;
        return false;
    };
    if (s.empty()) return fail("empty hotkey");
    unsigned m = 0;
    size_t start = 0;
    for (;;) {
        const size_t plus = s.find('+', start);
        const std::string tok = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (plus == std::string::npos) {
            const std::string name = KeysymName(tok);
            if (name.empty()) return fail("unknown key '" + tok + "'");
            const KeySym sym = XStringToKeysym(name.c_str());
            if (sym == NoSymbol) return fail("unknown key '" + tok + "'");
            if (mods) *mods = m;
            if (keysym) *keysym = (unsigned long)sym;
            return true;
        }
        const std::string t = Lower(tok);
        if (t == "ctrl" || t == "control") m |= ControlMask;
        else if (t == "shift") m |= ShiftMask;
        else if (t == "alt") m |= Mod1Mask;
        else if (t == "win" || t == "super") m |= Mod4Mask;
        else return fail("unknown modifier '" + tok + "'");
        start = plus + 1;
    }
}

bool X11HotkeysAvailable(std::string *why_not) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        if (why_not) {
            const char *env = std::getenv("DISPLAY");
            *why_not = std::string("cannot open the X display '") + (env ? env : "") + "'";
        }
        return false;
    }
    XCloseDisplay(d);
    return true;
}

std::unique_ptr<IHotkeyService> CreateX11HotkeyService(std::string *why_not) {
    Display *d = XOpenDisplay(nullptr);
    if (!d) {
        X11HotkeysAvailable(why_not);
        return nullptr;
    }
    return std::unique_ptr<IHotkeyService>(new X11HotkeyService(d));
}

}  // namespace platform
}  // namespace hr
