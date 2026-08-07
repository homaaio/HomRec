#pragma once
// -----------------------------------------------------------------------------
// hr_archive.h
//
// Zip-format archive extraction, used for .hrp plugin packages (a HomRec
// plugin file is just a .zip renamed to .hrp -- see lua_engine.h's
// long-standing note about this) and for the "External Overlay" input-
// overlay import (overlays_dock_panel.cpp), which also ships as a .hrp
// containing a .json layout + a .png spritesheet.
//
// No zip/inflate library is vendored here -- instead this shells out to a
// tool that's already on the machine:
//   1. tar.exe (bsdtar), built into Windows 10 1803+ / all of Windows 11,
//      lives in System32, needs no install. It sniffs the archive format
//      from its contents rather than trusting the extension, so it
//      extracts a .hrp exactly like a .zip with no renaming needed.
//   2. Falls back to PowerShell's Expand-Archive on older Windows where
//      tar.exe isn't present. Expand-Archive DOES insist on a .zip
//      extension, so in that path the file is first copied to a temp
//      "*.zip" path.
// -----------------------------------------------------------------------------
#include <string>

// Extracts the zip-format archive at archive_path into dest_dir (created,
// including any missing parent directories, if it doesn't already exist).
// archive_path's extension does not need to be .zip. Returns true on
// success (tool ran and exited 0); false otherwise (tool missing, bad
// archive, I/O error, etc.) -- check HrLog for the specifics.
bool HrExtractArchive(const std::string &archive_path, const std::string &dest_dir);
