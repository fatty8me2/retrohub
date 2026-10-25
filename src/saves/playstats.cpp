#include "playstats.h"

#include "savestore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace hub::saves {

std::string playlog_line(int64_t start, int64_t secs) { return std::to_string(start) + "," + std::to_string(secs) + "\n"; }

bool parse_playlog_line(const std::string& line, int64_t* start, int64_t* secs) {
    long long a = 0, b = 0;
    if (std::sscanf(line.c_str(), "%lld,%lld", &a, &b) != 2 || a <= 0 || b <= 0 || b > 7 * 86400) return false;
    *start = a;
    *secs = b;
    return true;
}

Stats compute_stats(const std::vector<GameTotal>& totals, const std::vector<Session>& sessions, int64_t now, int tz, size_t top_n) {
    Stats st;
    std::map<std::string, int64_t> sys;
    std::vector<StatsTop> top;
    for (auto& t : totals) {
        if (t.secs <= 0) continue;
        st.total_secs += t.secs;
        sys[t.system] += t.secs;
        top.push_back({t.system, t.key, t.secs});
    }
    std::stable_sort(top.begin(), top.end(), [](const StatsTop& a, const StatsTop& b) { return a.secs > b.secs; });
    if (top.size() > top_n) top.resize(top_n);
    st.top = top;
    for (auto& kv : sys) st.systems.push_back(kv);
    std::stable_sort(st.systems.begin(), st.systems.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

    std::set<int64_t> days;
    for (auto& s : sessions) {
        if (s.secs <= 0) continue;
        st.sessions++;
        if (s.start + s.secs > now - 7 * 86400 && s.start <= now) st.week_secs += s.secs;
        if (s.secs > st.longest.secs) st.longest = s;
        int64_t d = (s.start + tz);
        days.insert(d >= 0 ? d / 86400 : -((-d + 86399) / 86400));
    }
    int64_t t = now + tz;
    int64_t today = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
    int64_t d = days.count(today) ? today : today - 1;
    while (days.count(d)) { st.streak_days++; --d; }
    return st;
}

namespace {
std::string device_tag() {
    char b[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    std::string s = GetComputerNameA(b, &n) ? std::string(b, n) : std::string("pc");
    for (auto& c : s) if (!isalnum((unsigned char)c) && c != '-') c = '_';
    return s;
}
}  // namespace

void playlog_append(const std::string& system, const std::string& key, int64_t start, int64_t secs) {
    if (start <= 0 || secs <= 0) return;
    fs::path d = fs::u8path(game_dir(system, key)) / "playlog";
    std::error_code ec;
    fs::create_directories(d, ec);
    std::ofstream o(d / (device_tag() + ".log"), std::ios::app | std::ios::binary);
    if (o) o << playlog_line(start, secs);
}

std::vector<Session> playlog_read_all() {
    std::vector<Session> v;
    std::error_code ec;
    fs::path root = fs::u8path(vault_dir());
    for (fs::directory_iterator si(root, ec), se; !ec && si != se; si.increment(ec)) {
        std::string sys = si->path().filename().string();
        if (!si->is_directory(ec) || sys.empty() || sys[0] == '.' || sys[0] == '_') continue;
        std::error_code e2;
        for (fs::directory_iterator gi(si->path(), e2), ge; !e2 && gi != ge; gi.increment(e2)) {
            fs::path pl = gi->path() / "playlog";
            std::error_code e3;
            if (!fs::is_directory(pl, e3)) continue;
            std::string key = gi->path().filename().string();
            for (fs::directory_iterator fi(pl, e3), fe; !e3 && fi != fe; fi.increment(e3)) {
                std::ifstream in(fi->path(), std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    Session s;
                    s.system = sys;
                    s.key = key;
                    if (parse_playlog_line(line, &s.start, &s.secs)) v.push_back(s);
                }
            }
        }
    }
    return v;
}

} // namespace hub::saves
