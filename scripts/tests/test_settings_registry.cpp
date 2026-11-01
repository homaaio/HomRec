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
