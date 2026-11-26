// process_linux.cpp - IChildProcess on Linux: fork/exec + pipes (phase 1 of plan-linux.md).
// The Linux twin of what ffmpeg/ffmpeg_runner.cpp does with CreateProcess today.
//
// Behaviour worth knowing:
//   * RequestStop() sends SIGINT. ffmpeg treats it as "finish the file": the MP4 gets its index.
//     (Writing 'q' to stdin would be data when stdin carries raw frames, so it is not used.)
//   * The child gets its own process group, so Ctrl+C in the terminal reaches the parent only and
//     the parent decides when to stop the child. If the parent is killed with SIGKILL the child
//     is not stopped (stdin-piped children see EOF and end by themselves).
//   * stdin / stdout that are not piped are connected to /dev/null, so the child never reads the
//     terminal.
//   * WriteStdin() on a pipe whose reader has exited returns -1; it does not raise SIGPIPE.
//   * The destructor closes the pipes and, if the child still runs, kills and reaps it.
#include "../process_backend.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hr {
namespace platform {

namespace {

void CloseFd(int *fd) {
    if (*fd >= 0) { ::close(*fd); *fd = -1; }
}

void SleepMs(int ms) {
    timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

long long NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

class LinuxChildProcess : public IChildProcess {
public:
    ~LinuxChildProcess() override {
        CloseStdin();
        CloseFd(&out_fd_);
        if (pid_ > 0 && !Reap(false)) Kill();
    }

    bool Start(const SpawnSpec &spec, std::string *err) override {
        if (pid_ > 0) { if (err) *err = "process already started"; return false; }
        if (spec.exe.empty()) { if (err) *err = "no executable given"; return false; }

        // Everything the child needs is built BEFORE fork(): after it only async-signal-safe calls.
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(spec.exe.c_str()));
        for (const std::string &a : spec.args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        int in_pipe[2]  = {-1, -1};
        int out_pipe[2] = {-1, -1};
        int err_pipe[2] = {-1, -1};   // child -> parent: errno of a failed exec / setup step
        if (pipe2(err_pipe, O_CLOEXEC) != 0 ||
            (spec.pipe_stdin  && pipe2(in_pipe,  O_CLOEXEC) != 0) ||
            (spec.pipe_stdout && pipe2(out_pipe, O_CLOEXEC) != 0)) {
            if (err) *err = std::string("pipe2: ") + std::strerror(errno);
            CloseAll(in_pipe, out_pipe, err_pipe);
            return false;
        }

        const pid_t pid = fork();
        if (pid < 0) {
            if (err) *err = std::string("fork: ") + std::strerror(errno);
            CloseAll(in_pipe, out_pipe, err_pipe);
            return false;
        }

        if (pid == 0) {
            ChildMain(spec, argv.data(), in_pipe, out_pipe, err_pipe[1]);
            _exit(127);   // not reached
        }

        // parent
        ::close(err_pipe[1]);
        if (in_pipe[0]  >= 0) ::close(in_pipe[0]);
        if (out_pipe[1] >= 0) ::close(out_pipe[1]);

        int child_errno = 0;
        ssize_t n;
        do { n = ::read(err_pipe[0], &child_errno, sizeof(child_errno)); } while (n < 0 && errno == EINTR);
        ::close(err_pipe[0]);
        if (n == (ssize_t)sizeof(child_errno)) {
            // exec (or a setup step) failed in the child
            int st = 0;
            waitpid(pid, &st, 0);
            if (in_pipe[1]  >= 0) ::close(in_pipe[1]);
            if (out_pipe[0] >= 0) ::close(out_pipe[0]);
            if (err) *err = spec.exe + ": " + std::strerror(child_errno);
            return false;
        }

        pid_    = pid;
        in_fd_  = in_pipe[1];
        out_fd_ = out_pipe[0];
        return true;
    }

    long WriteStdin(const void *buf, size_t n) override {
        if (in_fd_ < 0) return -1;
        // A write to a pipe nobody reads raises SIGPIPE; block it for this call and swallow it.
        sigset_t set, old;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &old);

        const char *p = static_cast<const char *>(buf);
        size_t done = 0;
        bool broken = false;
        while (done < n) {
            const ssize_t w = ::write(in_fd_, p + done, n - done);
            if (w > 0) { done += (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            broken = true;
            break;
        }
        if (broken && errno == EPIPE) {
            timespec zero = {0, 0};
            sigtimedwait(&set, nullptr, &zero);
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
        return broken ? -1 : (long)done;
    }

    void CloseStdin() override { CloseFd(&in_fd_); }

    long ReadStdout(void *buf, size_t cap) override {
        if (out_fd_ < 0) return -1;
        for (;;) {
            const ssize_t r = ::read(out_fd_, buf, cap);
            if (r >= 0) return (long)r;
            if (errno != EINTR) return -1;
        }
    }

    void RequestStop() override {
        if (pid_ > 0 && !exited_) ::kill(pid_, SIGINT);
    }

    // timeout_ms < 0 waits without limit, 0 only looks once.
    bool Wait(int timeout_ms) override {
        if (pid_ <= 0) return exited_;
        const long long deadline = NowMs() + timeout_ms;
        for (;;) {
            if (Reap(timeout_ms < 0 ? true : false)) return true;
            if (timeout_ms >= 0 && NowMs() >= deadline) return false;
            SleepMs(5);
        }
    }

    void Kill() override {
        if (pid_ > 0 && !exited_) {
            ::kill(pid_, SIGKILL);
            Reap(true);
        }
    }

    bool IsRunning() const override {
        if (pid_ <= 0) return false;
        return !const_cast<LinuxChildProcess *>(this)->Reap(false);
    }

    int ExitCode() const override { return exit_code_; }

private:
    static void CloseAll(int *a, int *b, int *c) {
        for (int i = 0; i < 2; ++i) { CloseFd(&a[i]); CloseFd(&b[i]); CloseFd(&c[i]); }
    }

    // Collects the exit status. block=true waits for it. Returns true once the child is gone.
    bool Reap(bool block) {
        if (exited_) return true;
        if (pid_ <= 0) return false;
        int st = 0;
        pid_t r;
        do { r = waitpid(pid_, &st, block ? 0 : WNOHANG); } while (r < 0 && errno == EINTR);
        if (r == 0) return false;
        if (r < 0) { exited_ = true; exit_code_ = -1; return true; }   // ECHILD: already reaped elsewhere
        exited_ = true;
        exit_code_ = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
        return true;
    }

    // Runs in the forked child: only async-signal-safe calls below.
    static void ChildMain(const SpawnSpec &spec, char *const *argv, const int *in_pipe,
                          const int *out_pipe, int err_fd) {
        setpgid(0, 0);

        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        int fd_in = spec.pipe_stdin ? in_pipe[0] : ::open("/dev/null", O_RDONLY);
        if (fd_in < 0 || dup2(fd_in, 0) < 0) Fail(err_fd);

        int fd_out = spec.pipe_stdout ? out_pipe[1] : ::open("/dev/null", O_WRONLY);
        if (fd_out < 0 || dup2(fd_out, 1) < 0) Fail(err_fd);

        int fd_err;
        if (spec.stderr_path.empty()) fd_err = ::open("/dev/null", O_WRONLY);
        else fd_err = ::open(spec.stderr_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd_err < 0 || dup2(fd_err, 2) < 0) Fail(err_fd);

        // dup2 clears O_CLOEXEC on 0/1/2 only; the originals above 2 close themselves on exec.
        if (!spec.working_dir.empty() && chdir(spec.working_dir.c_str()) != 0) Fail(err_fd);
        if (spec.low_priority) { errno = 0; if (nice(10) == -1 && errno != 0) { /* not fatal */ } }

        execvp(spec.exe.c_str(), argv);
        Fail(err_fd);
    }

    [[noreturn]] static void Fail(int err_fd) {
        const int e = errno;
        ssize_t ignored = ::write(err_fd, &e, sizeof(e));
        (void)ignored;
        _exit(127);
    }

    pid_t pid_       = -1;
    int   in_fd_     = -1;
    int   out_fd_    = -1;
    bool  exited_    = false;
    int   exit_code_ = -1;
};

}  // namespace

std::unique_ptr<IChildProcess> CreateChildProcess() {
    return std::unique_ptr<IChildProcess>(new LinuxChildProcess());
}

}  // namespace platform
}  // namespace hr
