// scripts/tests/test_homrec_cli.cpp - unit tests for the portable parts of the homrec CLI:
// tools/homrec/homrec_tui.h (text pieces), homrec_overlay.h (overlay editing) and homrec_binding.h
// (the cfg/homrec_cli.cfg file).
//
// Build (any OS with a C++17 compiler):
//   g++ -std=c++17 -I src -I tools/homrec -I scripts/tests scripts/tests/test_homrec_cli.cpp -o t && ./t

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "homrec_binding.h"
#include "homrec_overlay.h"
#include "homrec_tui.h"

#include <fstream>

using namespace hrcli;

namespace {

// A fresh empty folder under the system temp dir, removed again at the end of the scope.
struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("homrec_cli_test_" + std::to_string(++counter) + "_" +
                                              std::to_string((long long)fs::file_time_type::clock::now().time_since_epoch().count()));
        fs::create_directories(path, ec);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::string Slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// tui
// ---------------------------------------------------------------------------------------------------

TEST_CASE("tui: Width counts code points, not bytes") {
    CHECK(tui::Width("abc") == 3);
    CHECK(tui::Width("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == 6);   // Russian "hello"
    CHECK(tui::Width("") == 0);
}

TEST_CASE("tui: Fit cuts the end, FitTail cuts the start, both respect the width") {
    CHECK(tui::Fit("short", 10, true) == "short");
    const std::string f = tui::Fit("a very long overlay name", 10, true);
    CHECK(tui::Width(f) == 10);
    CHECK(f.compare(0, 9, "a very lo") == 0);

    const std::string t = tui::FitTail("C:\\Users\\me\\Videos\\HomRec", 12, true);
    CHECK(tui::Width(t) == 12);
    CHECK(t.size() >= 3);
    CHECK(t.find("HomRec") != std::string::npos);

    // ASCII mode: the ellipsis is three dots and still fits
    const std::string a = tui::Fit("a very long overlay name", 10, false);
    CHECK(tui::Width(a) == 10);
    CHECK(a.substr(a.size() - 3) == "...");
}

TEST_CASE("tui: Fit never splits a multi-byte character") {
    const std::string ru = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80 \xD0\xBC\xD0\xB8\xD1\x80";
    const std::string f = tui::Fit(ru, 8, true);
    CHECK(tui::Width(f) == 8);
    // valid UTF-8: no continuation byte without a lead byte at the cut
    for (size_t i = 0; i < f.size(); ++i)
        if (((unsigned char)f[i] & 0xC0) == 0x80) CHECK(i > 0);
}

TEST_CASE("tui: FormatClock and FormatSize") {
    CHECK(tui::FormatClock(0) == "00:00:00");
    CHECK(tui::FormatClock(83.9) == "00:01:23");
    CHECK(tui::FormatClock(3725) == "01:02:05");
    CHECK(tui::FormatClock(-5) == "00:00:00");
    CHECK(tui::FormatSize(42.5) == "42.5 MB");
    CHECK(tui::FormatSize(2048) == "2.00 GB");
}

TEST_CASE("tui: RecLine shows pause, fps and dropped frames, and is identical for identical input") {
    const auto a = tui::RecLine(false, tui::Lang::En, false, 83, 42.5, 60, false);
    CHECK(a == tui::RecLine(false, tui::Lang::En, false, 83, 42.5, 60, false));
    CHECK(a.find("REC") != std::string::npos);
    CHECK(a.find("00:01:23") != std::string::npos);
    CHECK(a.find("42.5 MB") != std::string::npos);
    CHECK(a.find("60 fps") != std::string::npos);
    CHECK(a.find("dropping") == std::string::npos);

    const auto p = tui::RecLine(false, tui::Lang::En, true, 83, 42.5, 60, false);
    CHECK(p.find("PAUSED") != std::string::npos);
    CHECK(p.find("fps") == std::string::npos);   // no fps while paused

    CHECK(tui::RecLine(false, tui::Lang::En, false, 1, 1, 30, true).find("dropping frames") != std::string::npos);
    CHECK(tui::RecLine(true, tui::Lang::En, false, 1, 1, 30, false).find("\x1b[") != std::string::npos);
    CHECK(tui::RecLine(false, tui::Lang::En, false, 1, 1, 30, false).find("\x1b[") == std::string::npos);
}

TEST_CASE("tui: the saving bar keeps its width for every tick") {
    for (int tick = 0; tick < 100; ++tick) {
        const std::string plain = tui::SavingBar(tick, false);
        CHECK(plain.size() == (size_t)tui::kBarWidth + 2);
        CHECK(plain.front() == '[');
        CHECK(plain.back() == ']');
        size_t blocks = 0;
        for (char c : plain) if (c == '#') ++blocks;
        CHECK(blocks == (size_t)tui::kWaveWidth);
    }
    CHECK(tui::DoneBar(false).size() == (size_t)tui::kBarWidth + 2);
    CHECK(tui::SavingLine(false, tui::Lang::En, 0, 0.5, false).find("(") == std::string::npos);   // no timer under 2 s
    CHECK(tui::SavingLine(false, tui::Lang::En, 0, 3.0, false).find("(3.0s)") != std::string::npos);
}

TEST_CASE("tui: every message exists in both languages") {
    for (int i = 0; i <= (int)tui::Msg::Cancel; ++i) {
        const auto m = (tui::Msg)i;
        CHECK(std::string(tui::Tr(tui::Lang::En, m)).size() > 0);
        CHECK(std::string(tui::Tr(tui::Lang::Ru, m)).size() > 0);
    }
    CHECK(tui::LangFromCode("ru") == tui::Lang::Ru);
    CHECK(tui::LangFromCode("RU-ru") == tui::Lang::Ru);
    CHECK(tui::LangFromCode("en") == tui::Lang::En);
    CHECK(tui::LangFromCode("") == tui::Lang::En);
}

TEST_CASE("tui: menu rows differ only by the marker") {
    const auto sel = tui::MenuItemLine("Yes", true, false);
    const auto other = tui::MenuItemLine("Yes", false, false);
    CHECK(sel.find('>') != std::string::npos);
    CHECK(other.find('>') == std::string::npos);
    CHECK(tui::Width(sel) == tui::Width(other));
}

// ---------------------------------------------------------------------------------------------------
// binding / cfg file
// ---------------------------------------------------------------------------------------------------

TEST_CASE("binding: default when nothing is stored") {
    TempDir d;
    const Binding b = ReadBindingIn(d.path);
    CHECK(b.is_default);
    CHECK(b.path == (d.path / "homrec.hrc").lexically_normal());
    CHECK_FALSE(IsSetupDoneIn(d.path));
}

TEST_CASE("binding: writing one key keeps the others") {
    TempDir d;
    std::string err;
    const fs::path cfg = d.path / "configs" / "main.hrc";
    REQUIRE(WriteBindingIn(d.path, cfg, &err));
    REQUIRE(MarkSetupDoneIn(d.path, &err));
    CHECK(IsSetupDoneIn(d.path));
    CHECK(ReadBindingIn(d.path).path == cfg.lexically_normal());
    CHECK_FALSE(ReadBindingIn(d.path).is_default);

    // changing the config afterwards must not lose setup_done
    const fs::path other = d.path / "other.hrc";
    REQUIRE(WriteBindingIn(d.path, other, &err));
    CHECK(IsSetupDoneIn(d.path));
    CHECK(ReadBindingIn(d.path).path == other.lexically_normal());

    // one line per key
    const std::string text = Slurp(d.path / "cfg" / "homrec_cli.cfg");
    size_t n = 0, pos = 0;
    while ((pos = text.find("config=", pos)) != std::string::npos) { ++n; ++pos; }
    CHECK(n == 1);
}

TEST_CASE("binding: hand-edited file - BOM, comments, spaces, relative path, extra keys") {
    TempDir d;
    fs::create_directories(d.path / "cfg");
    {
        std::ofstream f(d.path / "cfg" / "homrec_cli.cfg", std::ios::binary);
        f << "\xEF\xBB\xBF# comment\r\n; another\r\n  config = sub/mine.hrc  \r\nlang=ru\r\nsetup_done=1\r\n";
    }
    const Binding b = ReadBindingIn(d.path);
    CHECK_FALSE(b.is_default);
    CHECK(b.path == (d.path / "sub" / "mine.hrc").lexically_normal());
    CHECK(IsSetupDoneIn(d.path));
    CHECK(ReadSettingIn(d.path, kLangKey) == "ru");

    std::string err;
    REQUIRE(WriteSettingIn(d.path, "setup_done", "0", &err));
    CHECK_FALSE(IsSetupDoneIn(d.path));
    CHECK(ReadSettingIn(d.path, kLangKey) == "ru");
}

// ---------------------------------------------------------------------------------------------------
// overlay editing
// ---------------------------------------------------------------------------------------------------

TEST_CASE("overlay: defaults match the Add buttons of the HomRec window") {
    const OverlayDef t = overlay::MakeDefault("text");
    CHECK(t.w == 200); CHECK(t.h == 60); CHECK(t.x == 40); CHECK(t.visible);
    const OverlayDef w = overlay::MakeDefault("webcam");
    CHECK(w.w == 240); CHECK(w.h == 180);
    const OverlayDef i = overlay::MakeDefault("image");
    CHECK(i.w == 200); CHECK(i.h == 150);
    CHECK(overlay::IsCreatableType("gif"));
    CHECK_FALSE(overlay::IsCreatableType("window"));
}

TEST_CASE("overlay: new overlays go on top with unique ids") {
    std::vector<OverlayDef> list;
    overlay::AddOverlay(list, overlay::MakeDefault("text"));
    overlay::AddOverlay(list, overlay::MakeDefault("image"));
    overlay::AddOverlay(list, overlay::MakeDefault("webcam"));
    REQUIRE(list.size() == 3);
    CHECK(list[0].type == "webcam");   // the newest is the first row = in front
    CHECK(list[2].type == "text");
    CHECK(list[0].id != list[1].id);
    CHECK(list[1].id != list[2].id);
    CHECK(list[0].id != list[2].id);

    // an id freed by a removal is reused, and never one that is taken
    const std::string freed = list[1].id;
    list.erase(list.begin() + 1);
    overlay::AddOverlay(list, overlay::MakeDefault("text"));
    CHECK(list[0].id == freed);
}

TEST_CASE("overlay: SetProp validates numbers, colours and booleans") {
    OverlayDef ov = overlay::MakeDefault("text");
    std::string err;
    CHECK(overlay::SetProp(ov, "x", "-20", &err));      CHECK(ov.x == -20);
    CHECK(overlay::SetProp(ov, "W", "640", &err));      CHECK(ov.w == 640);   // key is case-insensitive
    CHECK(overlay::SetProp(ov, "opacity", "55", &err)); CHECK(ov.opacity == 55);
    CHECK(overlay::SetProp(ov, "visible", "no", &err)); CHECK_FALSE(ov.visible);
    CHECK(overlay::SetProp(ov, "visible", "\xD0\xB4\xD0\xB0", &err)); CHECK(ov.visible);   // Russian "yes"
    CHECK(overlay::SetProp(ov, "color", "ff8800", &err)); CHECK(ov.text_color == "#FF8800");
    CHECK(overlay::SetProp(ov, "color", "#00ff00", &err)); CHECK(ov.text_color == "#00FF00");
    CHECK(overlay::SetProp(ov, "text", "Hello", &err)); CHECK(ov.text == "Hello");

    const OverlayDef before = ov;
    CHECK_FALSE(overlay::SetProp(ov, "w", "0", &err));
    CHECK_FALSE(overlay::SetProp(ov, "w", "abc", &err));
    CHECK_FALSE(overlay::SetProp(ov, "w", "12px", &err));
    CHECK_FALSE(overlay::SetProp(ov, "opacity", "101", &err));
    CHECK_FALSE(overlay::SetProp(ov, "opacity", "-1", &err));
    CHECK_FALSE(overlay::SetProp(ov, "color", "red", &err));
    CHECK_FALSE(overlay::SetProp(ov, "color", "#12345", &err));
    CHECK_FALSE(overlay::SetProp(ov, "visible", "maybe", &err));
    CHECK_FALSE(overlay::SetProp(ov, "text", "", &err));
    CHECK_FALSE(overlay::SetProp(ov, "text", "two\nlines", &err));
    CHECK_FALSE(overlay::SetProp(ov, "nonsense", "1", &err));
    CHECK(err.find("nonsense") != std::string::npos);
    // a failed change leaves the overlay as it was
    CHECK(ov.w == before.w); CHECK(ov.opacity == before.opacity); CHECK(ov.text_color == before.text_color);
    CHECK(ov.visible == before.visible); CHECK(ov.text == before.text);
}

TEST_CASE("overlay: properties that do not belong to the type are refused") {
    std::string err;
    OverlayDef img = overlay::MakeDefault("image");
    CHECK_FALSE(overlay::SetProp(img, "text", "x", &err));
    CHECK(err.find("image") != std::string::npos);
    CHECK_FALSE(overlay::SetProp(img, "color", "#FFFFFF", &err));

    OverlayDef txt = overlay::MakeDefault("text");
    CHECK_FALSE(overlay::SetProp(txt, "path", "whatever.png", &err));
    CHECK_FALSE(overlay::SetProp(txt, "webcam", "0", &err));

    OverlayDef cam = overlay::MakeDefault("webcam");
    cam.webcam_name = "Old camera";
    CHECK(overlay::SetProp(cam, "webcam", "1", &err));
    CHECK(cam.webcam_index == 1);
    CHECK(cam.webcam_name.empty());   // the name of the previous camera is dropped

    const auto text_props = overlay::PropsFor("text");
    bool has_color = false, has_path = false;
    for (const auto *p : text_props) { if (std::string(p->key) == "color") has_color = true; if (std::string(p->key) == "path") has_path = true; }
    CHECK(has_color);
    CHECK_FALSE(has_path);
}

TEST_CASE("overlay: file properties need an existing file") {
    TempDir d;
    const fs::path png = d.path / "pic.png";
    { std::ofstream f(png, std::ios::binary); f << "x"; }
    OverlayDef ov = overlay::MakeDefault("image");
    std::string err;
    CHECK(overlay::SetProp(ov, "path", png.u8string(), &err));
    CHECK(ov.image_path == png.u8string());
    CHECK_FALSE(overlay::SetProp(ov, "path", (d.path / "missing.png").u8string(), &err));
    CHECK(err.find("not found") != std::string::npos);
    CHECK(ov.image_path == png.u8string());
    CHECK_FALSE(overlay::SetProp(ov, "path", d.path.u8string(), &err));   // a folder is not a file
}

TEST_CASE("overlay: Find by row number, id and name") {
    std::vector<OverlayDef> list;
    overlay::AddOverlay(list, overlay::MakeDefault("text"));    // ends up as row 3
    overlay::AddOverlay(list, overlay::MakeDefault("image"));   // row 2
    overlay::AddOverlay(list, overlay::MakeDefault("webcam"));  // row 1
    list[0].name = "Face";
    list[1].name = "Logo";
    list[2].name = "face";   // same name as row 1, different case

    CHECK(overlay::Find(list, "1") == 0);
    CHECK(overlay::Find(list, "3") == 2);
    CHECK(overlay::Find(list, "4") == overlay::kNotFound);
    CHECK(overlay::Find(list, "0") == overlay::kNotFound);
    CHECK(overlay::Find(list, list[1].id) == 1);
    CHECK(overlay::Find(list, "logo") == 1);
    CHECK(overlay::Find(list, "FACE") == overlay::kAmbiguous);
    CHECK(overlay::Find(list, "nothing") == overlay::kNotFound);
    CHECK(overlay::Find(list, "") == overlay::kNotFound);
}

TEST_CASE("overlay: table rows fit the console and show the essentials") {
    OverlayDef ov = overlay::MakeDefault("text");
    ov.text = "A very long caption that does not fit into a narrow console window at all";
    for (size_t cols : {60u, 80u, 120u}) {
        const std::string row = overlay::Row(12, ov, false, cols, false);
        CHECK(tui::Width(row) <= cols);
        CHECK(row.find("text") != std::string::npos);
        CHECK(row.find("40,40") != std::string::npos);
        CHECK(row.find("200x60") != std::string::npos);
        CHECK(row.find("100%") != std::string::npos);
    }
    OverlayDef hidden = ov;
    hidden.visible = false;
    CHECK(overlay::Row(1, hidden, false, 80, false).find('o') != std::string::npos);
    CHECK(overlay::Row(1, ov, true, 80, false).find('>') != std::string::npos);
    CHECK(overlay::Row(1, ov, false, 80, false).find('>') == std::string::npos);

    ov.name = "Caption";
    CHECK(overlay::Row(1, ov, false, 80, false).find("Caption") != std::string::npos);
}

TEST_CASE("overlay: summary falls back to the content") {
    OverlayDef img = overlay::MakeDefault("image");
    img.image_path = "C:\\pics\\logo.png";
#ifdef _WIN32
    CHECK(overlay::Summary(img) == "logo.png");
#endif
    OverlayDef cam = overlay::MakeDefault("webcam");
    cam.webcam_index = 2;
    CHECK(overlay::Summary(cam) == "camera 2");
    cam.webcam_name = "HD Webcam";
    CHECK(overlay::Summary(cam) == "HD Webcam");
    OverlayDef t = overlay::MakeDefault("text");
    t.text = "Hi";
    CHECK(overlay::Summary(t) == "\"Hi\"");
}
