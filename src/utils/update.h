// update.h
//
// GitHub-releases-based update checker + self-updater for hr.exe. Wired up
// to Help > Check for Updates (ID_HELP_CHECK_UPDATES) in main_frame.cpp.
//
// The release flow this is built around:
//   1. Tag a GitHub release vX.Y.Z on homaaio/HomREC.
//   2. Attach the Inno Setup output (see installer/HomRec.iss) as a release
//      asset whose filename ends in .exe (e.g. HomRec-Setup-2.0.3.exe).
//   3. CheckForUpdate() compares HR_APP_VERSION (version.h) against the
//      latest release's tag_name and, if newer, returns that asset's
//      browser_download_url.
//   4. DownloadAndLaunchInstaller() fetches that asset to %TEMP% and runs
///     it with silent-install flags the Inno script understands (see
//      installer/HomRec.iss's [Setup] - AppMutex + /VERYSILENT means the
//      new installer closes the running app itself and overwrites in
//      place - this is "auto-update" without any separate updater binary).
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

// Runs CheckForUpdate() on a detached background thread and invokes
// `on_done` with the result. `on_done` is called from that background
// thread, NOT the UI thread - callers touching UI state from it must hop
// back themselves (e.g. wxWindow::CallAfter) before touching any widgets.
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
// Run it on a worker thread and poll `state` for progress. On success returns
// true and sets *out_path to the finished temp file; on failure/cancel returns
// false and leaves no partial file behind. Always sets state.finished last.
bool DownloadInstaller(const UpdateInfo &info, DownloadState &state, std::wstring *out_path);

// Starts a downloaded installer (from DownloadInstaller) with Inno Setup's
// silent-upgrade flags and returns immediately. Callers should close the app
// shortly after a successful call (the new installer can't overwrite hr.exe
// while it's still running, even with AppMutex set - AppMutex just stops the
// user launching a *second* copy mid-update). False = nothing was launched.
bool LaunchInstaller(const std::wstring &installer_path);

// Convenience wrapper: DownloadInstaller() + LaunchInstaller() in one
// blocking call with no progress or cancel. Kept for callers that are already
// on a worker thread; the UI uses the two functions above directly.
bool DownloadAndLaunchInstaller(const UpdateInfo &info);

} // namespace HrUpdate
