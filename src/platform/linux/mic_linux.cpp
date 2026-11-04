// mic_linux.cpp - HrEnumerateMics() (core/mic_enum.h) for Linux: the capture devices of the sound
// server, asked through `pactl` (PulseAudio, and PipeWire through pipewire-pulse, answer the same).
// This is only for the Settings > Audio picker; the real capture backend is a later phase.
#include "../../core/mic_enum.h"

#include <cstdio>
#include <string>

namespace {

bool EndsWith(const std::string &s, const char *suffix) {
    const std::string x = suffix;
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

std::string Trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

std::vector<HrMicDevice> HrEnumerateMics() {
    std::vector<HrMicDevice> out;
    // LC_ALL=C keeps the "Name:" / "Description:" keys in English whatever the user's language is.
    FILE *p = popen("LC_ALL=C pactl list sources 2>/dev/null", "r");
    if (!p) return out;

    HrMicDevice cur;
    auto flush = [&]() {
        // ".monitor" sources are the loopback of an output (what "Desktop audio" is), not microphones.
        if (!cur.id.empty() && !EndsWith(cur.id, ".monitor")) {
            if (cur.name.empty()) cur.name = cur.id;
            out.push_back(cur);
        }
        cur = HrMicDevice();
    };

    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p)) {
        const std::string line = Trim(buf);
        if (line.compare(0, 8, "Source #") == 0) flush();
        else if (line.compare(0, 6, "Name: ") == 0) cur.id = line.substr(6);
        else if (line.compare(0, 13, "Description: ") == 0) cur.name = line.substr(13);
    }
    flush();
    pclose(p);
    return out;
}
