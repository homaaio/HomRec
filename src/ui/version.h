// version.h
//
// Single source of truth for the app version string. hr_version.cpp and
// language.cpp's "app_title" both build off HR_APP_VERSION now, so there's
// only one place left to bump on release.
#pragma once

#define HR_APP_VERSION      "1.7.2"
#define HR_APP_VERSION_W    L"1.7.2"

#define HR_SINGLE_INSTANCE_MUTEX_NAME "HomRec_SingleInstance_150"
