// archive.h - zip extraction for .hrp plugin packages and External Overlay imports.
#pragma once
#include <string>

// Extracts the zip-format archive at archive_path into dest_dir (created,
// including any missing parent directories, if it doesn't already exist).
bool HrExtractArchive(const std::string &archive_path, const std::string &dest_dir);
