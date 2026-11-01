// process_backend.h - starting a child process with pipes. This is what ffmpeg/ffmpeg_runner.cpp
// does today with CreateProcess + anonymous pipes; on Linux it is fork/exec (posix_spawn) + pipe().
//
// HomRec drives ffmpeg in three ways that all need to survive the port:
//   1. raw frames in through stdin (video pipe), optionally a second pipe for audio / overlay
//   2. encoded stream out through stdout (Instant Replay's MPEG-TS ring, see replay_ram.h)
//   3. graceful stop: ask ffmpeg to finish the file (Windows: write 'q' / Ctrl+Break; Linux: 'q' on
//      stdin or SIGINT) and only kill after a timeout - a killed MP4 has no index.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hr {
namespace platform {

struct SpawnSpec {
    std::string              exe;                 // full path or a name to look up in PATH
    std::vector<std::string> args;                // argv[1..], NO shell quoting - passed as-is
    std::string              working_dir;         // "" = inherit
    bool                     pipe_stdin   = false;
    bool                     pipe_stdout  = false;
    std::string              stderr_path;         // "" = discard, otherwise appended to this file
    bool                     low_priority = false;  // nice(10) / BELOW_NORMAL
};

class IChildProcess {
public:
    virtual ~IChildProcess() = default;

    virtual bool Start(const SpawnSpec &spec, std::string *err) = 0;

    // Blocking write to the child's stdin; returns bytes written, -1 when the pipe is broken.
    virtual long WriteStdin(const void *buf, size_t n) = 0;
    virtual void CloseStdin() = 0;
    // Blocking read; returns bytes read, 0 at end of stream, -1 on error.
    virtual long ReadStdout(void *buf, size_t cap) = 0;

    virtual void RequestStop() = 0;               // polite: let the child finalise its output
    virtual bool Wait(int timeout_ms) = 0;        // true = the child has exited
    virtual void Kill() = 0;
    virtual bool IsRunning() const = 0;
    virtual int  ExitCode() const = 0;            // valid after Wait() returned true
};

std::unique_ptr<IChildProcess> CreateChildProcess();

}  // namespace platform
}  // namespace hr
