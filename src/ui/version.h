// version.h
//
// Single source of truth for the app version string. hr_app_logic.cpp's
// hr_version_string()/hr_version_gt() and language.cpp's "app_title" both
// build off HR_APP_VERSION now, so there's only one place left to bump on
// release. (The old hr_version.cpp duplicated this as its own constants
// and was never part of either build - removed as dead code.)
#pragma once

#define HR_APP_VERSION      "2.0.2"
#define HR_APP_VERSION_W    L"2.0.2"

#define HR_SINGLE_INSTANCE_MUTEX_NAME "HomRec_SingleInstance_150"
