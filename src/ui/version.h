// version.h — Phase 1
//
// AUDIT NOTE: the repo currently has THREE different version strings for
// the same release:
//   - homrec_app/core/constants.py   CURRENT_VERSION = "1.7.1"
//   - homrec_app/app.py log line     "HomRec v1.7.1"
//   - hr_version.cpp                 k_ver_homrec = L"1.7.0"
//   - README.md badge                1.7.2
//
// This header becomes the single source of truth for the UI layer. Two
// follow-up patches are needed outside this file (left for you to apply,
// since they touch files this phase doesn't otherwise change):
//   1. hr_version.cpp: change k_ver_homrec from L"1.7.0" to L"1.7.2"
//      (or whatever the intended real version is — the README is presumably
//      the most recently updated source, so 1.7.2 is the best guess, but
//      worth confirming with you).
//   2. Delete the "app_title" hardcoded "HomRec v1.7.1" duplication —
//      once the console/menu code is ported (later phases), the title bar
//      should build its string as `L"HomRec v" + kAppVersion` instead of
//      embedding the version inside the language table at all. Left as-is
//      in language.cpp for now so Phase 1 stays a faithful behavioral port;
//      flagging it here so it isn't forgotten.
#pragma once

#define HR_APP_VERSION      "1.7.2"
#define HR_APP_VERSION_W    L"1.7.2"

#define HR_SINGLE_INSTANCE_MUTEX_NAME "HomRec_SingleInstance_150"
