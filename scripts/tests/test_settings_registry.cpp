// scripts/tests/test_settings_registry.cpp - unit tests for src/core/settings_registry.cpp
// (the one table of every AppState setting: key, .hrc section, get/set as text).
//
// Build (any OS with a C++17 compiler):
//   g++ -std=c++17 -I src -I src/core -I scripts/tests scripts/tests/test_settings_registry.cpp src/core/settings_registry.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "settings_registry.h"

#include <set>
#include <string>

using namespace HrSettingsRegistry;

TEST_CASE("registry: table is not empty and every key is unique") {
    const auto &all = All();
    CHECK(all.size() > 50);
    std::set<std::string> seen;
    for (const auto &d : all) {
        CHECK_FALSE(d.key.empty());
        CHECK(d.get);
        CHECK(d.set);
        CHECK(seen.insert(d.key).second);
    }
}

TEST_CASE("registry: Find is case-insensitive and unknown keys give nullptr") {
    CHECK(Find("quality") != nullptr);
    CHECK(Find("QUALITY") == Find("quality"));
    CHECK(Find("no_such_setting_xyz") == nullptr);
}

TEST_CASE("registry: int field round-trips through text") {
    AppState st;
    const SettingDef *d = Find("quality");
    REQUIRE(d);
    CHECK(d->kind == Kind::Int);
    CHECK(d->set(st, "42"));
    CHECK(st.quality == 42);
    CHECK(d->get(st) == "42");
}

TEST_CASE("registry: bool field round-trips through text") {
    AppState st;
    const SettingDef *d = Find("disable_preview");
    REQUIRE(d);
    CHECK(d->kind == Kind::Bool);
    CHECK(d->set(st, "true"));
    CHECK(st.disable_preview);
    CHECK(d->set(st, "false"));
    CHECK_FALSE(st.disable_preview);
}

TEST_CASE("registry: string field round-trips through text") {
    AppState st;
    const SettingDef *d = Find("theme");
    REQUIRE(d);
    CHECK(d->set(st, "light"));
    CHECK(st.current_theme == "light");
    CHECK(d->get(st) == "light");
}

TEST_CASE("registry: auto-pause on idle keys exist, default to off, and round-trip") {
    AppState st;
    CHECK_FALSE(st.auto_pause_on_idle);              // the feature must never be on by itself

    const SettingDef *on = Find("auto_pause_on_idle");
    REQUIRE(on);
    CHECK(on->kind == Kind::Bool);
    CHECK(on->set(st, "true"));
    CHECK(st.auto_pause_on_idle);
    AppState st2;                                    // what get() writes, set() reads back
    CHECK(on->set(st2, on->get(st)));
    CHECK(st2.auto_pause_on_idle);

    // the aliases reach the same setting
    CHECK(Find("action_detection") == on);
    CHECK(Find("record_on_motion") == on);

    const SettingDef *sec = Find("idle_duration_sec");
    REQUIRE(sec);
    CHECK(sec->set(st, "12"));
    CHECK(st.idle_duration_sec == 12);

    const SettingDef *thr = Find("idle_threshold_pct");
    REQUIRE(thr);
    CHECK(thr->set(st, "0.25"));
    CHECK(st.idle_threshold_pct == doctest::Approx(0.25));
}
