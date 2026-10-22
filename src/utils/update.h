// update.h - GitHub-releases-based update checker + self-updater for hr.exe. Wired up
// to Help > Check for Updates (ID_HELP_CHECK_UPDATES) in main_frame.cpp.
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace HrUpdate {

struct UpdateInfo {
    bool checked_ok = false;      // false = network/parse failure; other fields meaningless
    bool available = false;       // true = latest_version is newer than HR_APP_VERSION
    std::string latest_version;   // e.g. "2.0.3" ('v' prefix already stripped)
    std::string download_url;     // .exe asset URL, or empty if the release has none
    std::string html_url;         // release page, always set when checked_ok - fallback link
};

// Dot-separated integer version compare (extra/missing components treated
// as 0, e.g. "2.1" == "2.1.0"). Returns true iff a > b.
bool VersionGreaterThan(const std::string &a, const std::string &b);

// Hits the GitHub API synchronously. Safe to call off the UI thread - it
// blocks on network I/O (WinHTTP), which is why CheckForUpdateAsync exists.
UpdateInfo CheckForUpdate();

// Runs CheckForUpdate() on a detached background thread and invokes `on_done` with the
// result.
void CheckForUpdateAsync(std::function<void(UpdateInfo)> on_done);

// Shared between the thread running DownloadInstaller() and the UI thread
// that shows the progress dialog - hence atomics. The UI thread only reads
// received/total/finished and writes cancel.
struct DownloadState {
    std::atomic<unsigned long long> received{0}; // bytes written to disk so far
    std::atomic<unsigned long long> total{0};    // Content-Length, 0 = server didn't say
    std::atomic<bool> cancel{false};             // UI sets this to abort the download
    std::atomic<bool> cancelled{false};          // download thread: stopped because of `cancel`
    std::atomic<bool> finished{false};           // download thread: DownloadInstaller() is done
};

// BLOCKS until the installer is fully downloaded (or failed / cancelled), so
// it must NOT be called on the UI thread - a multi-MB download there is what
// made the whole window "Not responding" during Help > Check for Updates.
bool DownloadInstaller(const UpdateInfo &info, DownloadState &state, std::wstring *out_path);

// Downloads any https URL that is a Windows executable (starts with "MZ", size matches
// Content-Length) to `dest_path`, reporting progress/cancel through `state` exactly like
// DownloadInstaller().
bool DownloadExeFile(const std::wstring &url, const std::wstring &dest_path, DownloadState *state);

// Starts a downloaded installer (from DownloadInstaller) with Inno Setup's silent-upgrade
// flags and returns immediately.
bool LaunchInstaller(const std::wstring &installer_path);

// Convenience wrapper: DownloadInstaller() + LaunchInstaller() in one
// blocking call with no progress or cancel. Kept for callers that are already
// on a worker thread; the UI uses the two functions above directly.
bool DownloadAndLaunchInstaller(const UpdateInfo &info);

} // namespace HrUpdate
