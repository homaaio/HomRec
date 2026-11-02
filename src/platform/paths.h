// paths.h - where HomRec keeps and finds things.
//
// Windows today is "portable": everything lives next to hr.exe (homrec.hrc, logs\, recordings\) and
// the installer puts the whole folder under Program Files / the user profile. Linux cannot work that
// way - /usr/bin is read-only - so the Linux rules follow the XDG Base Directory spec, while a
// "portable mode" (a file called `portable` next to the executable) keeps the Windows behaviour for
// tarballs and AppImages.
//
//                      Windows (today)        Linux (planned)
//   ConfigDir()        <exe dir>              $XDG_CONFIG_HOME/homrec   (~/.config/homrec)
//   DataDir()          <exe dir>              $XDG_DATA_HOME/homrec     (~/.local/share/homrec)
//   LogDir()           <exe dir>\logs         $XDG_STATE_HOME/homrec/logs (~/.local/state/homrec/logs)
//   VideosDir()        <exe dir>\recordings   $XDG_VIDEOS_DIR/HomRec or ~/Videos/HomRec
//
// All results are UTF-8, use '/' on Linux, have no trailing separator, and are NOT created -
// call EnsureDir() when you are about to write.
#pragma once

#include <string>

namespace hr {
namespace platform {

std::string ExecutableDir();       // directory of the running executable
std::string ConfigDir();
std::string DataDir();
std::string LogDir();
std::string VideosDir();           // default output folder for new recordings
bool        IsPortableMode();      // true -> every *Dir() above collapses into ExecutableDir()

// mkdir -p. True when the directory exists afterwards.
bool EnsureDir(const std::string &path);

// Looks for ffmpeg: $HOMREC_FFMPEG, then <exe dir>/ffmpeg, then $PATH. "" when not found.
std::string FindFfmpeg();

}  // namespace platform
}  // namespace hr
