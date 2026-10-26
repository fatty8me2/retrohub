// streamrun: remote play host / guest (see docs/REMOTE_PLAY.md).
#define SDL_MAIN_HANDLED
#include <cstdarg>
#include "media.h"
#include "session.h"
#include "stream_core.h"
#include "dsu_server.h"
#include "pad_convert.h"

#include <SDL.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rplay;

namespace {

const char* kNoDirect = "Couldn't connect directly to the host - remote play needs a direct connection (try another network / enable UPnP)";

struct Args {
    bool host = false, guest = false, headless = false, fullscreen = false, overlay = true, upnp = false, test_pattern = false;
    bool selftest = false, test_input = false, no_audio = false, pcm_audio = false, sw_enc = false, sw_dec = true;
    bool probe = false, test_menu = false, no_dsu = false; int menu_at = 3;
    std::string await_file;   // --await-pid-file: start the DSU server now, capture the PID written to this file later (Cemu must find the DSU server already up)
    std::string capture, net, room, stats_file, status_file;
    int slot = 0, pcs = 2, pad_port = 0 /* 0 = legacy localhost pad API off */, dsu_port = dsu::kDefaultPort, bitrate = 8000, fps = 60, max_height = 720, duration = 0, probe_ms = 9000;
};

std::atomic<bool> g_stop{false};
BOOL WINAPI on_ctrl(DWORD) { g_stop = true; return TRUE; }

/* Clean-quit request from the hub: it sets the named event "Local\RetroHubStreamrunQuit_<pid>" instead of TerminateProcess, so the
 * guest sends its bye (the host reports the pad gone at once) and the host tells every guest the stream ended. */
HANDLE g_quit_ev = nullptr;
void make_quit_event() {
    char name[96]; snprintf(name, sizeof name, "Local\\RetroHubStreamrunQuit_%lu", (unsigned long)GetCurrentProcessId());
    g_quit_ev = CreateEventA(nullptr, TRUE, FALSE, name);
}
bool quit_requested() { return g_quit_ev && WaitForSingleObject(g_quit_ev, 0) == WAIT_OBJECT_0; }

uint32_t now32() { return (uint32_t)steady_us(); }

void logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    static FILE* lf = [] { const char* p = getenv("STREAMRUN_LOG"); return p && *p ? fopen(p, "ab") : nullptr; }();   // QA: console-less children (hub-spawned) log here
    if (lf) { fprintf(lf, "[%lu] %s\n", (unsigned long)GetCurrentProcessId(), buf); fflush(lf); }
}

bool parse_args(int argc, char** argv, Args* a)
{
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto val = [&](std::string* o) { if (i + 1 >= argc) return false; *o = argv[++i]; return true; };
        auto ival = [&](int* o) { std::string t; if (!val(&t)) return false; *o = atoi(t.c_str()); return true; };
        if (s == "--host") a->host = true;
        else if (s == "--selftest") a->selftest = true;
        else if (s == "--guest") a->guest = true;
        else if (s == "--headless") a->headless = true;
        else if (s == "--fullscreen") a->fullscreen = true;
        else if (s == "--overlay") a->overlay = true;
        else if (s == "--no-overlay") a->overlay = false;
        else if (s == "--upnp") a->upnp = true;
        else if (s == "--test-pattern") a->test_pattern = true;
        else if (s == "--test-input") a->test_input = true;
        else if (s == "--test-menu") a->test_menu = true;
        else if (s == "--test-menu-at") { if (!ival(&a->menu_at)) return false; }
        else if (s == "--probe") a->probe = true;
        else if (s == "--no-dsu") a->no_dsu = true;
        else if (s == "--dsu-port") { if (!ival(&a->dsu_port)) return false; }
        else if (s == "--probe-ms") { if (!ival(&a->probe_ms)) return false; }
        else if (s == "--await-pid-file") { if (!val(&a->await_file)) return false; }
        else if (s == "--status-file") { if (!val(&a->status_file)) return false; }
        else if (s == "--no-audio") a->no_audio = true;
        else if (s == "--pcm-audio") a->pcm_audio = true;
        else if (s == "--sw-encode") a->sw_enc = true;
        else if (s == "--sw-decode") a->sw_dec = true;
        else if (s == "--hw-decode") a->sw_dec = false;
        else if (s == "--capture-hwnd") { if (!val(&a->capture)) return false; }
        else if (s == "--net") { if (!val(&a->net)) return false; }
        else if (s == "--room") { if (!val(&a->room)) return false; }
        else if (s == "--stats-file") { if (!val(&a->stats_file)) return false; }
        else if (s == "--slot") { if (!ival(&a->slot)) return false; }
        else if (s == "--pcs") { if (!ival(&a->pcs)) return false; }
        else if (s == "--guests") { int g; if (!ival(&g)) return false; a->pcs = g + 1; }
        else if (s == "--pad-port") { if (!ival(&a->pad_port)) return false; }
        else if (s == "--bitrate") { if (!ival(&a->bitrate)) return false; }
        else if (s == "--fps") { if (!ival(&a->fps)) return false; }
        else if (s == "--max-height") { if (!ival(&a->max_height)) return false; }
        else if (s == "--duration") { if (!ival(&a->duration)) return false; }
        else { fprintf(stderr, "unknown argument %s\n", s.c_str()); return false; }
    }
    if (a->selftest) return true;
    if (a->host == a->guest) return false;
    if (a->net.empty() || a->room.empty()) return false;
    if (a->host && a->slot != 0) return false;
    if (a->guest && a->slot < 1) return false;
    if (a->host && !a->test_pattern && !a->probe && a->capture.empty() && a->await_file.empty()) return false;
    return true;
}

void usage()
{
    fprintf(stderr,
        "usage: streamrun --host (--capture-hwnd <hwnd|pid> | --test-pattern) --net <wss://server> --room <CODE-N> --slot 0 [--pcs 2]\n"
        "                 [--await-pid-file F (instead of --capture-hwnd)] [--dsu-port 26760] [--no-dsu] [--status-file F] [--pad-port P (legacy RPAD datagrams)] [--bitrate 8000] [--fps 60] [--max-height 720] [--no-audio] [--pcm-audio] [--sw-encode] [--upnp]\n"
        "       streamrun --guest --net <wss://server> --room <CODE-N> --slot K [--pcs 2] [--fullscreen] [--no-overlay] [--headless]\n"
        "                 [--test-input] [--test-menu] [--hw-decode] [--duration S] [--stats-file F]\n"
        "       streamrun --probe (--host|--guest) --net <wss://server> --room <CODE-P> --slot K [--pcs N] [--probe-ms 9000] [--stats-file F]\n"
        "                 direct-UDP reachability check, no media; exit 0 = every guest reachable, 3 = not\n");
}

double pct(std::vector<double>& v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

int session_fail_code(hubnet::Session& s, bool relay_disabled)
{
    std::string st = s.status();
    logf("session failed: %s", st.c_str());
    bool no_direct = relay_disabled && (st.find("Couldn't connect to the other players") != std::string::npos ||
                                        st.find("never joined") != std::string::npos);
    if (no_direct) { fprintf(stderr, "%s\n", kNoDirect); return 3; }
    return 4;
}

bool start_session(hubnet::Session& s, const Args& a, bool upnp_thread, std::thread* upnp)
{
    s.set_no_relay(true);
    hubnet::SessionConfig c;
    c.server = a.net; c.room = a.room; c.slot = a.slot; c.expected = a.pcs; c.ready_timeout_ms = 25000;
    if (!s.start(c)) { logf("session start failed: %s", s.status().c_str()); return false; }
    if (upnp_thread) {
        uint16_t port = s.local_port();
        *upnp = std::thread([port] { std::string info; bool ok = upnp_map_udp(port, &info); logf("upnp: %s (%s)", ok ? "mapped" : "not mapped", info.c_str()); });
    }
    return true;
}

// ============================================================================ HOST

struct OutPkt { int slot; Bytes data; };

struct HostState {
    std::mutex m;
    std::deque<OutPkt> video, audio, retx;
    NackHistory hist{120};
    std::atomic<bool> want_key{true};
    std::atomic<int> kbps{8000}, fec{8}, active{0};
    std::atomic<uint64_t> frames{0}, bytes{0}, enc_us_sum{0}, enc_max_us{0}, audio_pkts{0}, pads{0}, nacks{0}, retx_pkts{0}, keys{0};
    std::atomic<int> w{0}, h{0};
    std::string enc_name, cap_name, aud_name;
    bool enc_hw = false;
};

void host_video_thread(const Args* a, HostState* hs, CaptureSource* src, int enc_w, int enc_h, H264Encoder* enc)
{
    uint32_t frame_id = 1, seq = 0; int cur_kbps = a->bitrate;
    uint64_t last_cap = 0, period = 1000000 / std::max(1, a->fps);
    while (!g_stop) {
        BgraFrame f;
        if (!src->grab(&f, 100)) continue;
        if (last_cap && f.cap_us - last_cap < period * 7 / 10) continue;   // cap at ~fps
        last_cap = f.cap_us;
        if (hs->active.load() == 0) continue;   // nobody is watching
        if (const char* shot = getenv("STREAMRUN_SHOT")) {   // QA: dump one captured frame (BMP) ~5 s in, to see what the capture really contains
            static int cnt = 0;
            if (++cnt == 300) if (FILE* bf = fopen(shot, "wb")) {
                BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof ih; ih.biWidth = f.w; ih.biHeight = -f.h; ih.biPlanes = 1; ih.biBitCount = 32;
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + (DWORD)f.px.size();
                fwrite(&fh, sizeof fh, 1, bf); fwrite(&ih, sizeof ih, 1, bf); fwrite(f.px.data(), 1, f.px.size(), bf); fclose(bf);
            }
        }
        int kb = hs->kbps.load();
        if (kb != cur_kbps) { enc->set_bitrate(kb); cur_kbps = kb; }
        bool key = hs->want_key.exchange(false);
        std::vector<EncodedFrame> out;
        uint64_t t0 = steady_us();
        if (!enc->encode(f, key, &out)) { logf("encode failed"); continue; }
        uint64_t dt = steady_us() - t0;
        hs->enc_us_sum += dt; if (dt > hs->enc_max_us) hs->enc_max_us = dt;
        for (auto& e : out) {
            FrameInfo fi; fi.cap_us = (uint32_t)e.cap_us; fi.w = (uint16_t)enc_w; fi.h = (uint16_t)enc_h; fi.flags = e.key ? 1 : 0; fi.seq = seq++;
            Bytes blob = make_frame_blob(fi, e.data.data(), e.data.size());
            auto pk = packetize_frame(frame_id, e.key, blob, hs->fec.load());
            {
                std::lock_guard<std::mutex> g(hs->m);
                hs->hist.store(frame_id, pk);
                for (auto& p : pk) hs->video.push_back({-1, std::move(p)});
            }
            frame_id++; hs->frames++; hs->bytes += e.data.size(); if (e.key) hs->keys++;
        }
    }
}

void host_audio_thread(const Args* a, HostState* hs, AudioCapture* cap)
{
    AacEncoder aac; std::string err; bool use_aac = !a->pcm_audio && aac.init(&err);
    if (!use_aac) logf("audio: %s", a->pcm_audio ? "PCM requested" : ("AAC unavailable (" + err + "), using PCM").c_str());
    uint32_t seq = 0; std::vector<int16_t> pcm, acc;
    uint32_t t0 = now32();
    while (!g_stop) {
        pcm.clear();
        cap->read(&pcm, 20);
        if (pcm.empty() || hs->active.load() == 0) continue;
        uint32_t ts = (now32() - t0) / 1000;
        if (use_aac) {
            std::vector<std::vector<uint8_t>> frames;
            aac.encode(pcm.data(), pcm.size() / 2, &frames);
            std::lock_guard<std::mutex> g(hs->m);
            for (auto& f : frames) { if (f.size() + kAudioHdr > (size_t)kMaxDatagram) continue; hs->audio.push_back({-1, make_audio_pkt(kAudAac, seq++, ts, f.data(), f.size())}); hs->audio_pkts++; }
        } else {
            acc.insert(acc.end(), pcm.begin(), pcm.end());
            std::lock_guard<std::mutex> g(hs->m);
            while (acc.size() >= 480) {   // 240 frames = 5 ms = 960 bytes
                hs->audio.push_back({-1, make_audio_pkt(kAudPcm, seq++, ts, (const uint8_t*)acc.data(), 960)});
                acc.erase(acc.begin(), acc.begin() + 480); hs->audio_pkts++;
            }
        }
    }
}

int run_host(const Args& a_in)
{
    Args a = a_in;
    HostState hs; hs.kbps = a.bitrate;
    /* The DSU server comes up BEFORE Cemu is started: Cemu's DSU client does not retry once its first connect attempt failed. */
    dsu::DsuServer dsus; bool dsu_on = false;
    if (!a.no_dsu) {
        dsu_on = dsus.start((uint16_t)a.dsu_port, true);
        logf("host: DSU server on 127.0.0.1:%d %s", a.dsu_port, dsu_on ? "listening" : "FAILED to bind (guest pads will not reach Cemu)");
    }
    if (!a.await_file.empty() && !a.test_pattern) {   // the hub writes Cemu's PID here once the game window exists
        uint32_t t0w = hubnet::now_ms();
        while (a.capture.empty() && !g_stop && !quit_requested() && hubnet::now_ms() - t0w < 180000) {
            if (FILE* f = fopen(a.await_file.c_str(), "rb")) { char b[64] = {0}; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); if (n) { a.capture = b; while (!a.capture.empty() && (a.capture.back() == '\n' || a.capture.back() == '\r' || a.capture.back() == ' ')) a.capture.pop_back(); } }
            if (a.capture.empty()) Sleep(100);
        }
        if (a.capture.empty()) { logf("host: no capture target was given"); return g_stop || quit_requested() ? 0 : 5; }
        logf("host: capture target %s", a.capture.c_str());
    }
    std::unique_ptr<CaptureSource> src; uint32_t pid = 0;
    if (a.test_pattern) src = make_test_pattern(1280, 720, a.fps);
    else {
        void* hwnd = resolve_window(a.capture, &pid);
        if (!hwnd) { fprintf(stderr, "no window found for '%s'\n", a.capture.c_str()); return 5; }
        std::string err;
        src = make_window_capture(hwnd, a.fps, &err);
        if (!src) { fprintf(stderr, "capture failed: %s\n", err.c_str()); return 5; }
        if (!err.empty()) logf("capture: %s", err.c_str());
    }
    hs.cap_name = src->name();
    BgraFrame first;
    {
        uint64_t t0 = steady_us();
        while (!src->grab(&first, 200)) if (steady_us() - t0 > 8000000) { fprintf(stderr, "capture produced no frames\n"); return 5; }
    }
    int ew = first.w, eh = first.h;
    if (eh > a.max_height) { ew = (int)((int64_t)first.w * a.max_height / first.h); eh = a.max_height; }
    ew &= ~1; eh &= ~1;
    H264Encoder enc; std::string err;
    if (!enc.init(ew, eh, a.fps, a.bitrate, !a.sw_enc, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 5; }
    hs.enc_name = enc.name(); hs.enc_hw = enc.hardware();
    hs.w = ew; hs.h = eh;
    logf("host: capture=%s encoder=%s%s %dx%d@%d %d kbps", src->name().c_str(), enc.name().c_str(), enc.hardware() ? " (hw)" : "", ew, eh, a.fps, a.bitrate);

    std::unique_ptr<AudioCapture> acap;
    if (!a.no_audio) {
        std::string aerr;
        if (a.test_pattern) acap = make_tone_source();
        else if (pid) acap = make_process_loopback(pid, &aerr);
        if (!acap && !a.test_pattern) logf("audio capture unavailable: %s (video only)", aerr.c_str());
        if (acap) hs.aud_name = acap->name();
    }

    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET padsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in pad_to = {}; pad_to.sin_family = AF_INET; pad_to.sin_port = htons((u_short)a.pad_port); inet_pton(AF_INET, "127.0.0.1", &pad_to.sin_addr);

    hubnet::Session sess; std::thread up;
    if (!start_session(sess, a, a.upnp, &up)) return 4;
    logf("host: waiting for guests in room %s ...", a.room.c_str());

    std::thread vt, at; bool threads = false;
    BitrateController brc(a.bitrate);
    std::map<int, uint32_t> guests;   // slot -> last hello ms
    uint16_t pad_seq = 0; uint64_t bucket_us = steady_us(); double budget = 0;
    uint32_t t_start = hubnet::now_ms(); int rc = 0;
    StatsReport last_stats; uint32_t last_stat_ms = 0;
    std::map<int, StatsReport> gstats; std::map<int, uint32_t> gpads, left_until;   // per-guest receiver report / pad packets seen
    uint32_t last_status_ms = 0; uint64_t status_bytes = 0; int status_kbps = 0;
    auto send_gone = [&](int slot) {
        if (dsu_on) { int ds = dsu_slot_for_guest(slot); if (ds >= 0) dsus.set_connected(ds, false); }   // Cemu sees the pad disconnect
        logf("host: guest slot %d gone (pad disconnected)", slot);
        if (a.pad_port <= 0) return;
        PadApiPacket pk = {}; memcpy(pk.magic, "RGON", 4); pk.version = 1; pk.pad.buttons = 0xFFFF; pk.pad.slot = (uint8_t)slot;
        sendto(padsock, (const char*)&pk, sizeof pk, 0, (sockaddr*)&pad_to, sizeof pad_to);
    };
    auto write_status = [&](uint32_t now) {
        if (a.status_file.empty() || now - last_status_ms < 1000) return;
        double secs = last_status_ms ? (now - last_status_ms) / 1000.0 : 1.0; last_status_ms = now;
        uint64_t by = hs.bytes.load(); status_kbps = (int)((by - status_bytes) * 8.0 / 1000.0 / secs); status_bytes = by;
        std::string j = "{\"t\":" + std::to_string(now) + ",\"kbps\":" + std::to_string(status_kbps) + ",\"target_kbps\":" + std::to_string(hs.kbps.load()) +
                        ",\"dsu_clients\":" + std::to_string(dsu_on ? dsus.clients() : 0) + ",\"guests\":[";
        bool first = true;
        for (auto& g : guests) {
            int rtt = sess.ping_ms(g.first); auto st = gstats.find(g.first);
            if (rtt < 0 && st != gstats.end()) rtt = st->second.rtt_ms;
            j += std::string(first ? "" : ",") + "{\"slot\":" + std::to_string(g.first) + ",\"rtt_ms\":" + std::to_string(rtt) + ",\"loss_permille\":" +
                 std::to_string(st != gstats.end() ? st->second.loss_permille : 0) + ",\"pads\":" + std::to_string(gpads[g.first]) + "}";
            first = false;
        }
        j += "]}\n";
        std::string tmp = a.status_file + ".tmp";
        if (FILE* f = fopen(tmp.c_str(), "wb")) { fwrite(j.data(), 1, j.size(), f); fclose(f); MoveFileExA(tmp.c_str(), a.status_file.c_str(), MOVEFILE_REPLACE_EXISTING); }
    };

    while (!g_stop) {
        sess.update();
        uint32_t now = hubnet::now_ms();
        if (sess.failed()) { rc = session_fail_code(sess, true); break; }
        if (quit_requested()) { logf("host: quit requested"); break; }
        // sess.ended() is ignored on the host: a guest's clean bye is handled per slot (kPktBye) and the host session and Cemu carry on
        if (a.duration && now - t_start > (uint32_t)a.duration * 1000 + (sess.ready() ? 0 : 25000)) break;
        if (sess.ready() && !threads) {
            threads = true;
            vt = std::thread(host_video_thread, &a, &hs, src.get(), ew, eh, &enc);
            if (acap) at = std::thread(host_audio_thread, &a, &hs, acap.get());
            logf("host: session ready (relay never used), streaming on first guest hello");
        }
        int from; std::vector<uint8_t> d;
        while (sess.recv(&from, &d)) {
            if (d.empty()) continue;
            switch (d[0]) {
            case kPktHello:
                if (!guests.count(from)) { logf("host: guest slot %d joined", from); hs.want_key = true; }
                guests[from] = now; break;
            case kPktKeyReq: hs.want_key = true; guests[from] = now; break;
            case kPktBye:
                if (guests.erase(from)) { logf("host: guest slot %d left", from); send_gone(from); }
                left_until[from] = now + 3000;
                break;
            case kPktInput: {
                uint16_t sq; PadState ps;
                if (parse_input_pkt(d.data(), (int)d.size(), &sq, &ps)) {
                    ps.slot = (uint8_t)from;
                    auto lu = left_until.find(from);
                    if (lu != left_until.end() && (int32_t)(now - lu->second) < 0) break;   // input still in flight from a guest that just said bye
                    int ds = dsu_slot_for_guest(from);
                    if (dsu_on && ds >= 0) dsus.set_pad(ds, to_dsu(ps));
                    if (a.pad_port > 0) {
                        PadApiPacket pk = {}; memcpy(pk.magic, "RPAD", 4); pk.version = 1; pk.seq = pad_seq++; pk.host_ms = now; pk.pad = ps;
                        sendto(padsock, (const char*)&pk, sizeof pk, 0, (sockaddr*)&pad_to, sizeof pad_to);
                    }
                    if (!guests.count(from)) { guests[from] = now; hs.want_key = true; logf("host: guest slot %d joined (input)", from); }
                    else guests[from] = now;
                    gpads[from]++;
                    hs.pads++;
                }
                break; }
            case kPktNack: {
                auto m = parse_nack(d.data(), (int)d.size());
                std::lock_guard<std::mutex> g(hs.m);
                for (auto& f : m) {
                    hs.nacks++;
                    const Bytes* p = hs.hist.find(f.frame_id, f.idx);
                    if (p) { Bytes c = *p; c[1] |= kVfRetx; hs.retx.push_back({from, std::move(c)}); hs.retx_pkts++; }
                }
                break; }
            case kPktStats: {
                StatsReport s;
                if (parse_stats_pkt(d.data(), (int)d.size(), &s)) {
                    last_stats = s; last_stat_ms = now; gstats[from] = s;
                    hs.kbps = brc.update(s); hs.fec = brc.fec_group();
                }
                break; }
            }
        }
        for (auto it = guests.begin(); it != guests.end();) {
            if (now - it->second > 4000) { logf("host: guest slot %d timed out", it->first); send_gone(it->first); it = guests.erase(it); }
            else ++it;
        }
        hs.active = (int)guests.size();
        write_status(now);
        // paced send: audio and retransmits first, then video
        uint64_t t = steady_us();
        double rate = std::max(30000.0, hs.kbps.load() * 4.0) * 1000.0 / 8.0 / 1e6;   // bytes per microsecond
        budget = std::min(budget + (double)(t - bucket_us) * rate, 24000.0); bucket_us = t;
        for (;;) {
            OutPkt p;
            {
                std::lock_guard<std::mutex> g(hs.m);
                std::deque<OutPkt>* q = !hs.audio.empty() ? &hs.audio : !hs.retx.empty() ? &hs.retx : !hs.video.empty() ? &hs.video : nullptr;
                if (!q) break;
                if (q != &hs.audio && budget < 0) break;
                p = std::move(q->front()); q->pop_front();
            }
            if (hs.active.load() == 0) continue;
            budget -= (double)p.data.size();
            sess.send(p.slot, p.data.data(), (int)p.data.size());
        }
        Sleep(1);
    }
    g_stop = true;
    if (vt.joinable()) vt.join();
    if (at.joinable()) at.join();
    for (auto& g : guests) send_gone(g.first);
    sess.say_bye(); for (int i = 0; i < 5; i++) { sess.update(); Sleep(10); }
    sess.stop();
    if (up.joinable()) up.join();
    uint64_t fr = hs.frames.load();
    logf("host: frames=%llu keys=%llu bytes=%llu avg_enc=%.2fms max_enc=%.2fms pads=%llu nacks=%llu retx=%llu",
         (unsigned long long)fr, (unsigned long long)hs.keys.load(), (unsigned long long)hs.bytes.load(), fr ? hs.enc_us_sum.load() / 1000.0 / fr : 0.0,
         hs.enc_max_us.load() / 1000.0, (unsigned long long)hs.pads.load(), (unsigned long long)hs.nacks.load(), (unsigned long long)hs.retx_pkts.load());
    if (!a.stats_file.empty()) {
        FILE* f = fopen(a.stats_file.c_str(), "w");
        if (f) {
            fprintf(f, "{\"role\":\"host\",\"capture\":\"%s\",\"encoder\":\"%s\",\"encoder_hw\":%s,\"width\":%d,\"height\":%d,\"frames\":%llu,\"keyframes\":%llu,\"bytes\":%llu,"
                       "\"avg_encode_ms\":%.3f,\"max_encode_ms\":%.3f,\"audio_packets\":%llu,\"pad_packets\":%llu,\"nacks\":%llu,\"retx\":%llu,\"final_kbps\":%d,\"guest_loss_permille\":%d}\n",
                    hs.cap_name.c_str(), hs.enc_name.c_str(), hs.enc_hw ? "true" : "false", hs.w.load(), hs.h.load(), (unsigned long long)fr, (unsigned long long)hs.keys.load(),
                    (unsigned long long)hs.bytes.load(), fr ? hs.enc_us_sum.load() / 1000.0 / fr : 0.0, hs.enc_max_us.load() / 1000.0,
                    (unsigned long long)hs.audio_pkts.load(), (unsigned long long)hs.pads.load(), (unsigned long long)hs.nacks.load(), (unsigned long long)hs.retx_pkts.load(),
                    hs.kbps.load(), last_stats.loss_permille);
            fclose(f);
        }
    }
    return rc;
}

// ============================================================================ GUEST

const uint8_t kFont[][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},{5,5,7,1,1},{7,4,6,1,6},{3,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,6},   // 0-9
    {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},
    {6,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,7,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7},   // A-Z
    {0,0,0,0,2},{0,2,0,2,0},{1,1,2,4,4},{0,0,7,0,0},{5,1,2,4,5},
};
const uint8_t* glyph(char c)
{
    static const uint8_t blank[5] = {0, 0, 0, 0, 0};
    if (c >= '0' && c <= '9') return kFont[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return kFont[10 + c - 'A'];
    switch (c) { case '.': return kFont[36]; case ':': return kFont[37]; case '/': return kFont[38]; case '-': return kFont[39]; case '%': return kFont[40]; }
    return blank;
}
void draw_text(SDL_Renderer* r, int x, int y, int sc, const std::string& s)
{
    for (char c : s) {
        const uint8_t* g = glyph(c);
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4 >> col)) { SDL_Rect q = {x + col * sc, y + row * sc, sc, sc}; SDL_RenderFillRect(r, &q); }
        x += 4 * sc;
    }
}

struct GuestState {
    std::mutex dm; std::condition_variable dcv;
    std::deque<std::pair<Bytes, bool>> dq;   // blobs for the decoder
    std::mutex fm; DecodedFrame latest; uint64_t latest_seq = 0;
    std::mutex am; JitterBuffer jb{3, 24}; AacDecoder aac; bool aac_ok = false; uint8_t acodec = 0;
    std::vector<int16_t> pcm_ring;
    std::mutex pm; PadState pad{};
    std::mutex lm; std::vector<double> lat_ms, dec_ms;
    std::atomic<uint64_t> frames_decoded{0}, frames_shown{0}, bytes_rx{0}, audio_rx{0}, audio_samples{0}, audio_underrun{0}, keys_req{0}, decode_skips{0}, nacks_tx{0}, inputs_tx{0}, decode_fail{0};
    std::atomic<int> fps{0}, w{0}, h{0};
    std::atomic<bool> fatal{false}; std::atomic<int> fatal_code{0};
    std::atomic<bool> got_video{false}, hw_dec{false}, waiting_key{true};
    std::string dec_name;
    Args a;
    std::atomic<int> lat_last_ms{0};
    std::mutex mem; std::deque<MenuEv> menu_evs;   // View+Y / D-pad / A / B edges from the pad thread (or F1 / arrows / Enter from the window)
    std::atomic<bool> menu_open{false};            // while open the pad sends neutral input so menu presses don't reach the game
};

void record_latency(GuestState* gs, uint32_t cap32)
{
    double ms = (double)(uint32_t)(now32() - cap32) / 1000.0;
    gs->lat_last_ms = (int)ms;
    std::lock_guard<std::mutex> g(gs->lm);
    gs->lat_ms.push_back(ms);
}

void guest_decode_thread(GuestState* gs)
{
    auto decp = std::make_unique<H264Decoder>(); std::string err;
    bool d3d = !gs->a.sw_dec;
    if (!decp->init(d3d, &err)) { logf("decoder init failed: %s", err.c_str()); gs->fatal = true; gs->fatal_code = 5; return; }
    gs->hw_dec = decp->hardware();
    int bad = 0;
    while (!g_stop) {
        std::pair<Bytes, bool> item;
        {
            std::unique_lock<std::mutex> g(gs->dm);
            gs->dcv.wait_for(g, std::chrono::milliseconds(50), [&] { return !gs->dq.empty() || g_stop; });
            if (gs->dq.empty()) continue;
            if (gs->dq.size() > 4) {   // decoder is behind (slow GPU/CPU): jump to the newest keyframe, or resync via a keyframe request
                size_t k = gs->dq.size();
                while (k > 0 && !gs->dq[k - 1].second) k--;
                gs->decode_skips++;
                if (k > 0) gs->dq.erase(gs->dq.begin(), gs->dq.begin() + (k - 1));
                else { gs->dq.clear(); gs->waiting_key = true; gs->keys_req++; continue; }
            }
            item = std::move(gs->dq.front()); gs->dq.pop_front();
        }
        FrameInfo fi; const uint8_t* au; size_t len;
        if (!parse_frame_blob(item.first, &fi, &au, &len)) continue;
        std::vector<DecodedFrame> out;
        uint64_t t0 = steady_us();
        bool ok = decp->decode(au, len, fi.cap_us, &out);
        double dms = (steady_us() - t0) / 1000.0;
        if (!ok) {
            gs->decode_fail++;
            if (++bad == 5 && !gs->a.sw_dec && decp->hardware()) {   // hardware path broken: restart in software
                logf("hardware decode failing, switching to software");
                decp = std::make_unique<H264Decoder>(); decp->init(false, &err); gs->hw_dec = false; gs->waiting_key = true; gs->keys_req++;
            }
            continue;
        }
        { std::lock_guard<std::mutex> g(gs->lm); gs->dec_ms.push_back(dms); }
        for (auto& f : out) {
            gs->frames_decoded++; gs->w = f.w; gs->h = f.h;
            uint32_t cap32 = (uint32_t)f.cap_us;
            if (gs->a.headless) { record_latency(gs, cap32); gs->frames_shown++; }
            std::lock_guard<std::mutex> g(gs->fm);
            gs->latest = std::move(f); gs->latest_seq++;
        }
    }
}

// fills `samples` interleaved s16 frames*2 into dst; returns nothing, zero-fills underruns
void audio_fill(GuestState* gs, int16_t* dst, int n)
{
    std::lock_guard<std::mutex> g(gs->am);
    // refill ring from the jitter buffer
    while ((int)gs->pcm_ring.size() < n) {
        AudioPkt p; bool lost;
        if (!gs->jb.pop(&p, &lost)) break;
        if (lost) { gs->pcm_ring.insert(gs->pcm_ring.end(), gs->acodec == kAudAac ? 2048 : 480, 0); continue; }
        if (p.codec == kAudAac) {
            if (gs->aac_ok) gs->aac.decode(p.data.data(), p.data.size(), &gs->pcm_ring);
        } else if (p.codec == kAudPcm) {
            size_t o = gs->pcm_ring.size(); gs->pcm_ring.resize(o + p.data.size() / 2); memcpy(&gs->pcm_ring[o], p.data.data(), p.data.size() / 2 * 2);
        }
    }
    int have = (int)std::min<size_t>(gs->pcm_ring.size(), (size_t)n);
    if (have < n) gs->audio_underrun++;
    if (have) memcpy(dst, gs->pcm_ring.data(), (size_t)have * 2);
    if (have < n) memset(dst + have, 0, (size_t)(n - have) * 2);
    gs->pcm_ring.erase(gs->pcm_ring.begin(), gs->pcm_ring.begin() + have);
    gs->audio_samples += have / 2;
    const size_t cap = 48000 * 2 / 8;   // never hold more than 125 ms
    if (gs->pcm_ring.size() > cap) gs->pcm_ring.erase(gs->pcm_ring.begin(), gs->pcm_ring.end() - cap);
}
void SDLCALL sdl_audio_cb(void* ud, Uint8* stream, int len) { audio_fill((GuestState*)ud, (int16_t*)stream, len / 2); }

void guest_pad_thread(GuestState* gs)
{
    SDL_GameController* gc = nullptr; uint64_t n = 0; uint64_t last_scan = 0;
    uint64_t next = steady_us(); uint64_t t_begin = steady_us();
    MenuPadEdge edge;
    while (!g_stop) {
        PadState p = {};
        bool m_back = false, m_y = false, m_up = false, m_down = false, m_ok = false, m_cancel = false;
        if (gs->a.test_input) {
            p.buttons = (uint16_t)((n / 40) & 0x7FFF); p.lx = (int16_t)(((n * 97) % 65535) - 32768); p.ly = (int16_t)(n & 0x7FFF); p.lt = (uint8_t)(n & 255);
        } else if (gc || steady_us() - last_scan > 1000000) {
            SDL_LockJoysticks();
            if (!gc) {
                last_scan = steady_us();
                for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { gc = SDL_GameControllerOpen(i); if (gc) {
                    if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO)) SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE);
                    if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL)) SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE);
                    logf("guest: controller '%s'", SDL_GameControllerName(gc)); break; } }
            }
            if (gc) {
                SDL_GameControllerUpdate();
                if (!SDL_GameControllerGetAttached(gc)) { SDL_GameControllerClose(gc); gc = nullptr; }
                else {
                    auto B = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gc, b) != 0; };
                    auto A = [&](SDL_GameControllerAxis x) { return (int)SDL_GameControllerGetAxis(gc, x); };
                    // positional (Nintendo) layout: the east button is A, south is B, north X, west Y
                    if (B(SDL_CONTROLLER_BUTTON_B)) p.buttons |= kPadA;
                    if (B(SDL_CONTROLLER_BUTTON_A)) p.buttons |= kPadB;
                    if (B(SDL_CONTROLLER_BUTTON_Y)) p.buttons |= kPadX;
                    if (B(SDL_CONTROLLER_BUTTON_X)) p.buttons |= kPadY;
                    if (B(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) p.buttons |= kPadL;
                    if (B(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p.buttons |= kPadR;
                    if (B(SDL_CONTROLLER_BUTTON_START)) p.buttons |= kPadPlus;
                    if (B(SDL_CONTROLLER_BUTTON_BACK)) p.buttons |= kPadMinus;
                    if (B(SDL_CONTROLLER_BUTTON_GUIDE)) p.buttons |= kPadHome;
                    if (B(SDL_CONTROLLER_BUTTON_DPAD_UP)) p.buttons |= kPadUp;
                    if (B(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.buttons |= kPadDown;
                    if (B(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.buttons |= kPadLeft;
                    if (B(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.buttons |= kPadRight;
                    if (B(SDL_CONTROLLER_BUTTON_LEFTSTICK)) p.buttons |= kPadStickL;
                    if (B(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) p.buttons2 |= kPad2StickR;
                    p.lx = (int16_t)A(SDL_CONTROLLER_AXIS_LEFTX); p.ly = (int16_t)std::clamp(-A(SDL_CONTROLLER_AXIS_LEFTY), -32768, 32767);
                    p.rx = (int16_t)A(SDL_CONTROLLER_AXIS_RIGHTX); p.ry = (int16_t)std::clamp(-A(SDL_CONTROLLER_AXIS_RIGHTY), -32768, 32767);
                    p.lt = (uint8_t)(A(SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7); p.rt = (uint8_t)(A(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
                    if (p.lt > 128) p.buttons |= kPadZL;
                    if (p.rt > 128) p.buttons |= kPadZR;
                    m_back = B(SDL_CONTROLLER_BUTTON_BACK); m_y = B(SDL_CONTROLLER_BUTTON_Y);
                    m_up = B(SDL_CONTROLLER_BUTTON_DPAD_UP); m_down = B(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                    m_ok = B(SDL_CONTROLLER_BUTTON_A); m_cancel = B(SDL_CONTROLLER_BUTTON_B);
                    SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, p.gyro, 3);
                    SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, p.accel, 3);
                }
            }
            SDL_UnlockJoysticks();
        }
        if (gs->a.test_menu) {   // scripted presses (no pad on a hidden desktop): chord, then Down/OK x3 = stats, fullscreen, leave; 600 ms per step from t=3 s
            if (!gs->got_video) t_begin = steady_us();   // the script starts --test-menu-at seconds after the first video frame
            int64_t ms = (int64_t)(steady_us() - t_begin) / 1000 - (int64_t)gs->a.menu_at * 1000;
            if (ms >= 0) {
                int step = (int)(ms / 600); bool held = (ms % 600) < 300;
                if (held) switch (step) { case 0: m_back = m_y = true; break; case 1: case 3: case 5: m_down = true; break; case 2: case 4: case 6: m_ok = true; break; }
            }
        }
        MenuEv mev;
        if (edge.poll(m_back, m_y, m_up, m_down, m_ok, m_cancel, &mev)) { std::lock_guard<std::mutex> g(gs->mem); gs->menu_evs.push_back(mev); }
        if (gs->menu_open.load()) p = PadState{};   // the menu owns the pad
        n++;
        { std::lock_guard<std::mutex> g(gs->pm); gs->pad = p; }
        next += 1000000 / kInputHz;
        uint64_t t = steady_us();
        if (next > t) std::this_thread::sleep_for(std::chrono::microseconds(next - t)); else if (t - next > 50000) next = t;
    }
    if (gc) SDL_GameControllerClose(gc);
}

void guest_net_thread(GuestState* gs, hubnet::Session* sess)
{
    const Args& a = gs->a;
    Reassembler ra; uint16_t in_seq = 0;
    uint32_t t_start = hubnet::now_ms(), last_hello = 0, last_input = 0, last_stats = 0, last_nack = 0, last_key = 0;
    uint32_t last_frame_ms = 0; double jit = 0, mean_dt = 16.7;
    uint64_t prev_pk = 0, prev_bytes = 0, prev_frames = 0, prev_fec = 0; uint64_t nack_frags = 0, nack_prev = 0;
    bool started = false;
    while (!g_stop) {
        sess->update();
        uint32_t now = hubnet::now_ms();
        if (quit_requested()) { logf("guest: quit requested"); gs->fatal = true; break; }
        if (sess->failed()) { gs->fatal_code = session_fail_code(*sess, true); gs->fatal = true; break; }
        if (sess->ended()) { logf("guest: host ended the session"); gs->fatal_code = 0; gs->fatal = true; break; }
        if (a.duration && now - t_start > (uint32_t)a.duration * 1000 + (sess->ready() ? 0 : 25000)) { gs->fatal = true; break; }
        if (!sess->ready()) { Sleep(1); continue; }
        if (!started) { started = true; logf("guest: session ready (direct UDP, relay disabled)"); last_hello = now - 1000; }
        int from; std::vector<uint8_t> d;
        while (sess->recv(&from, &d)) {
            if (d.empty()) continue;
            if (d[0] == kPktVideo) {
                gs->bytes_rx += d.size();
                ra.feed(d.data(), (int)d.size(), now);
            } else if (d[0] == kPktAudio) {
                AudioPkt p;
                if (parse_audio_pkt(d.data(), (int)d.size(), &p)) {
                    gs->bytes_rx += d.size(); gs->audio_rx++;
                    std::lock_guard<std::mutex> g(gs->am);
                    if (gs->acodec != p.codec) { gs->acodec = p.codec; gs->jb.set_target(p.codec == kAudAac ? 3 : 10); if (p.codec == kAudAac && !gs->aac_ok) { std::string e; gs->aac_ok = gs->aac.init(&e); if (!gs->aac_ok) logf("guest: AAC decoder: %s", e.c_str()); } }
                    gs->jb.push(p);
                }
            }
        }
        // completed frames -> decoder
        uint32_t id; Bytes blob; bool key;
        while (ra.pop(&id, &blob, &key)) {
            gs->got_video = true;
            double dt = (double)(now - last_frame_ms); if (last_frame_ms) { jit = jit * 0.9 + std::abs(dt - mean_dt) * 0.1; mean_dt = mean_dt * 0.95 + dt * 0.05; }
            last_frame_ms = now;
            if (gs->waiting_key) { if (!key) continue; gs->waiting_key = false; }
            { std::lock_guard<std::mutex> g(gs->dm); if (gs->dq.size() > 120) gs->dq.clear(); gs->dq.emplace_back(std::move(blob), key); }
            gs->dcv.notify_one();
        }
        if (now - last_nack >= 4) {
            last_nack = now;
            int rtt = sess->ping_ms(0); uint32_t age = (uint32_t)std::clamp(rtt > 0 ? rtt / 2 + 6 : 10, 6, 60);
            auto m = ra.nacks(now, age, (uint32_t)std::max(25, rtt * 2 + 10), 48);
            if (!m.empty() && getenv("STREAMRUN_DEBUG")) { static int dbg = 0; if (dbg++ < 12) { std::string s; for (auto& x : m) s += std::to_string(x.frame_id) + ":" + std::to_string(x.idx) + " "; logf("nack age=%u rtt=%d now=%u: %s", age, rtt, now, s.c_str()); } }
            if (!m.empty()) { Bytes nk = make_nack(m); sess->send(0, nk.data(), (int)nk.size()); nack_frags += m.size(); gs->nacks_tx++; }
            ra.expire(now, 120);
        }
        if (ra.need_key) { ra.need_key = false; gs->waiting_key = true; gs->keys_req++; }
        if (gs->keys_req.load() > 0 && (gs->waiting_key.load()) && now - last_key >= 150 && gs->got_video) { last_key = now; uint8_t k = kPktKeyReq; sess->send(0, &k, 1); }
        if (now - last_hello >= (gs->got_video ? 1000u : 200u)) { last_hello = now; uint8_t h[2] = {kPktHello, (uint8_t)kProtoVersion}; sess->send(0, h, 2); }
        if (now - last_input >= 1000 / kInputHz) {
            last_input = now;
            PadState p; { std::lock_guard<std::mutex> g(gs->pm); p = gs->pad; }
            p.slot = (uint8_t)a.slot;
            Bytes ip = make_input_pkt(in_seq++, p); sess->send(0, ip.data(), (int)ip.size()); gs->inputs_tx++;
        }
        if (now - last_stats >= 1000) {
            double secs = (last_stats ? (now - last_stats) : 1000) / 1000.0; last_stats = now;
            uint64_t pk = ra.packets, by = gs->bytes_rx.load(), fr = gs->frames_decoded.load();
            uint64_t dpk = pk - prev_pk;
            uint64_t lossish = (ra.recovered_by_fec - prev_fec) + (nack_frags - nack_prev);
            StatsReport s; s.loss_permille = (uint16_t)(dpk ? std::min<uint64_t>(1000, lossish * 1000 / dpk) : 0);
            s.recv_kbps = (uint16_t)std::min<double>(65535, (by - prev_bytes) * 8.0 / 1000.0 / secs); s.jitter_ms = (uint16_t)std::min(65535.0, jit);
            s.fps = (uint8_t)std::min<double>(255, (fr - prev_frames) / secs); s.rtt_ms = (uint16_t)std::max(0, sess->ping_ms(0));
            gs->fps = s.fps;
            prev_pk = pk; prev_bytes = by; prev_frames = fr; prev_fec = ra.recovered_by_fec; nack_prev = nack_frags;
            if (gs->got_video) { Bytes sp = make_stats_pkt(s); sess->send(0, sp.data(), (int)sp.size()); }
        }
        Sleep(1);
    }
    if (started && !sess->ended()) { uint8_t b = kPktBye; for (int i = 0; i < 4; i++) { sess->send(0, &b, 1); sess->update(); Sleep(5); } logf("guest: sent bye"); }   // the host drops our pad at once
    sess->say_bye(); for (int i = 0; i < 5; i++) { sess->update(); Sleep(10); }
    gs->nacks_tx += 0;
    logf("guest: reassembler packets=%llu frames=%llu dropped=%llu fec_recovered=%llu dup=%llu nack_frags=%llu", (unsigned long long)ra.packets, (unsigned long long)ra.frames_done,
         (unsigned long long)ra.frames_dropped, (unsigned long long)ra.recovered_by_fec, (unsigned long long)ra.dup_packets, (unsigned long long)nack_frags);
    g_stop = true;
}

/* Reachability probe (no media): the host expects `pcs` PCs, every guest joins the same room, and each guest slot counts as
 * reachable once a direct (never relayed) ping answered. Host writes {"slots":{"1":rtt,...}} to --stats-file and exits 0 only when all
 * guests are reachable; a guest exits 0 once connected to the host. The relay is disabled (Session::set_no_relay). */
int run_probe(const Args& a)
{
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    hubnet::Session sess; std::thread up;
    Args b = a; (void)b;
    if (!start_session(sess, a, a.upnp, &up)) return 4;
    uint32_t t0 = hubnet::now_ms(); std::map<int, int> rtt; int rc = 3; uint32_t all_ok_at = 0;
    while (!g_stop) {
        sess.update(); uint32_t now = hubnet::now_ms();
        if (quit_requested() || now - t0 > (uint32_t)a.probe_ms) break;
        if (a.host) {
            bool all = a.pcs > 1;
            for (int sl = 1; sl < a.pcs; sl++) {
                int r = sess.ping_ms(sl);
                if (r >= 0 && !sess.is_relayed(sl)) rtt[sl] = r; else all = false;
            }
            if (all) { if (!all_ok_at) all_ok_at = now; if (now - all_ok_at > 1200) { rc = 0; break; } }   // hold a moment so the guests see the host answer too
        } else {
            int r = sess.ping_ms(0);
            if (sess.ready() && r >= 0) { if (!all_ok_at) all_ok_at = now; rc = 0; if (sess.ended() || now - all_ok_at > 6000) break; }
            if (sess.ended()) break;
        }
        Sleep(5);
    }
    if (a.host && !a.stats_file.empty()) {
        std::string j = "{\"ok\":" + std::string(rc == 0 ? "true" : "false") + ",\"slots\":{";
        for (int sl = 1; sl < a.pcs; sl++) { j += (sl > 1 ? "," : "") + std::string("\"") + std::to_string(sl) + "\":" + std::to_string(rtt.count(sl) ? rtt[sl] : -1); }
        j += "}}\n";
        if (FILE* f = fopen(a.stats_file.c_str(), "wb")) { fwrite(j.data(), 1, j.size(), f); fclose(f); }
        fputs(j.c_str(), stderr);
    }
    sess.say_bye(); for (int i = 0; i < 5; i++) { sess.update(); Sleep(10); }
    sess.stop(); if (up.joinable()) up.join();
    logf("probe (%s): %s", a.host ? "host" : "guest", rc == 0 ? "direct connection OK" : "no direct connection");
    return rc;
}

int run_guest(const Args& a)
{
    GuestState gs; gs.a = a;
    Uint32 flags = SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS | (a.headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO));
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(flags) != 0) { logf("SDL_Init: %s (continuing without controller)", SDL_GetError()); if (!a.headless) return 5; }
    SDL_Window* win = nullptr; SDL_Renderer* ren = nullptr; SDL_Texture* tex = nullptr; int tw = 0, th = 0;
    SDL_AudioDeviceID adev = 0;
    if (!a.headless) {
        SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");
        win = SDL_CreateWindow("RetroHub remote play", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                               SDL_WINDOW_RESIZABLE | (a.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!win) { logf("window: %s", SDL_GetError()); return 5; }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        if (!ren) { logf("renderer: %s", SDL_GetError()); return 5; }
        SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT709);
        SDL_AudioSpec want = {}, have = {};
        want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512; want.callback = sdl_audio_cb; want.userdata = &gs;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) SDL_PauseAudioDevice(adev, 0); else logf("audio device: %s", SDL_GetError());
    }
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    hubnet::Session sess; std::thread up;
    if (!start_session(sess, a, a.upnp, &up)) return 4;
    logf("guest: joining room %s as slot %d", a.room.c_str(), a.slot);

    std::thread nt(guest_net_thread, &gs, &sess), dt(guest_decode_thread, &gs), pt(guest_pad_thread, &gs);
    bool overlay = a.overlay; uint64_t shown_seq = 0; uint64_t t_start = steady_us();
    GuestMenu menu; bool menu_dirty = false;
    auto menu_event = [&](MenuEv ev) {
        MenuAct act = menu.handle(ev); gs.menu_open = menu.open; menu_dirty = true;
        switch (act) {
            case MenuAct::Opened: logf("guest: menu opened"); break;
            case MenuAct::Closed: logf("guest: menu closed"); break;
            case MenuAct::ToggleStats: overlay = !overlay; logf("guest: menu stats %s", overlay ? "on" : "off"); break;
            case MenuAct::ToggleFullscreen:
                if (win) SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                logf("guest: menu fullscreen toggled"); break;
            case MenuAct::Leave: logf("guest: menu leave"); g_stop = true; break;
            default: break;
        }
    };
    uint64_t next_audio = steady_us();
    std::vector<int16_t> sink(960);
    uint64_t fps_t = steady_us(), fps_n = 0; int shown_fps = 0;
    while (!g_stop) {
        if (quit_requested()) { logf("guest: quit requested"); g_stop = true; break; }
        { std::deque<MenuEv> evs; { std::lock_guard<std::mutex> g(gs.mem); evs.swap(gs.menu_evs); } for (MenuEv ev : evs) menu_event(ev); }
        if (!a.headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) g_stop = true;
                else if (e.type == SDL_KEYDOWN) {
                    if (e.key.keysym.sym == SDLK_F1) menu_event(MenuEv::Open);
                    else if (menu.open && e.key.keysym.sym == SDLK_UP) menu_event(MenuEv::Up);
                    else if (menu.open && e.key.keysym.sym == SDLK_DOWN) menu_event(MenuEv::Down);
                    else if (menu.open && (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE)) menu_event(MenuEv::Ok);
                    else if (menu.open && e.key.keysym.sym == SDLK_ESCAPE) menu_event(MenuEv::Back);
                    else if (e.key.keysym.sym == SDLK_F3) overlay = !overlay;
                    else if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))
                        SDL_SetWindowFullscreen(win, (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    else if (e.key.keysym.sym == SDLK_ESCAPE && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) SDL_SetWindowFullscreen(win, 0);
                }
            }
            DecodedFrame f; bool have_new = false;
            {
                std::lock_guard<std::mutex> g(gs.fm);
                if (gs.latest_seq != shown_seq) { f = std::move(gs.latest); shown_seq = gs.latest_seq; have_new = true; }
            }
            if (have_new || (menu_dirty && tex)) {
                menu_dirty = false;
                if (have_new && (!tex || tw != f.w || th != f.h)) {
                    if (tex) SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING, f.w, f.h); tw = f.w; th = f.h;
                }
                if (have_new) SDL_UpdateNVTexture(tex, nullptr, f.nv12.data(), f.w, f.nv12.data() + (size_t)f.w * f.h, f.w);
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
                int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
                double sc = std::min((double)ow / tw, (double)oh / th);
                SDL_Rect dst = {(int)((ow - tw * sc) / 2), (int)((oh - th * sc) / 2), (int)(tw * sc), (int)(th * sc)};
                SDL_RenderCopy(ren, tex, nullptr, &dst);
                if (overlay) {
                    char buf[160];
                    snprintf(buf, sizeof buf, "FPS %d  LAT %d MS  %d KBPS  RTT %d MS  %s", shown_fps, gs.lat_last_ms.load(), (int)(gs.bytes_rx.load() * 8 / 1000 / std::max<uint64_t>(1, (steady_us() - t_start) / 1000000)), sess.ping_ms(0), gs.hw_dec ? "HW" : "SW");
                    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160); SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    SDL_Rect bg = {4, 4, (int)strlen(buf) * 8 + 8, 24}; SDL_RenderFillRect(ren, &bg);
                    SDL_SetRenderDrawColor(ren, 80, 255, 120, 255); draw_text(ren, 8, 8, 2, buf);
                }
                if (menu.open) {   // View+Y menu: Resume / Stats / Fullscreen / Leave
                    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
                    int pw = 360, ph = 200, px = (ow - pw) / 2, py = (oh - ph) / 2;
                    SDL_SetRenderDrawColor(ren, 10, 12, 20, 225); SDL_Rect pn = {px, py, pw, ph}; SDL_RenderFillRect(ren, &pn);
                    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255); draw_text(ren, px + 16, py + 12, 3, "REMOTE PLAY");
                    for (int i = 0; i < GuestMenu::kItems; i++) {
                        if (i == menu.sel) { SDL_SetRenderDrawColor(ren, 60, 120, 255, 255); SDL_Rect hl = {px + 8, py + 48 + i * 34, pw - 16, 30}; SDL_RenderFillRect(ren, &hl); }
                        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255); draw_text(ren, px + 20, py + 54 + i * 34, 3, GuestMenu::item_name(i));
                    }
                }
                SDL_RenderPresent(ren);
                if (have_new) { record_latency(&gs, (uint32_t)f.cap_us); gs.frames_shown++; }
                fps_n++;
                if (steady_us() - fps_t >= 1000000) { shown_fps = (int)fps_n; fps_n = 0; fps_t += 1000000; }
            } else SDL_Delay(1);
        } else {
            // headless: emulate the audio device pulling 10 ms every 10 ms
            uint64_t t = steady_us();
            if (t >= next_audio) { audio_fill(&gs, sink.data(), 960); next_audio += 10000; if (t > next_audio + 100000) next_audio = t; }
            Sleep(2);
        }
        if (gs.fatal) break;
    }
    g_stop = true;
    gs.dcv.notify_all();
    nt.join(); dt.join(); pt.join();
    if (up.joinable()) up.join();
    sess.stop();
    // report
    std::vector<double> lat, dec;
    { std::lock_guard<std::mutex> g(gs.lm); lat = gs.lat_ms; dec = gs.dec_ms; }
    // ignore the first second (keyframe, warmup)
    size_t skip = std::min<size_t>(lat.size() / 10, 60);
    std::vector<double> l2(lat.begin() + skip, lat.end());
    double avg = 0; for (double v : l2) avg += v; if (!l2.empty()) avg /= l2.size();
    double secs = std::max(1.0, (steady_us() - t_start) / 1e6);
    double p50 = pct(l2, 0.5), p95 = pct(l2, 0.95), p99 = pct(l2, 0.99), mx = l2.empty() ? 0 : l2.back();
    double dec_avg = 0; for (double v : dec) dec_avg += v; if (!dec.empty()) dec_avg /= dec.size();
    logf("guest: shown=%llu decoded=%llu fps=%.1f latency_ms avg=%.1f p50=%.1f p95=%.1f p99=%.1f max=%.1f decode_avg=%.2fms recv=%.0f kbps inputs=%llu audio_pkts=%llu underruns=%llu",
         (unsigned long long)gs.frames_shown.load(), (unsigned long long)gs.frames_decoded.load(), gs.frames_shown.load() / secs, avg, p50, p95, p99, mx, dec_avg,
         gs.bytes_rx.load() * 8.0 / 1000.0 / secs, (unsigned long long)gs.inputs_tx.load(), (unsigned long long)gs.audio_rx.load(), (unsigned long long)gs.audio_underrun.load());
    if (!a.stats_file.empty()) {
        FILE* f = fopen(a.stats_file.c_str(), "w");
        if (f) {
            fprintf(f, "{\"role\":\"guest\",\"headless\":%s,\"hw_decode\":%s,\"width\":%d,\"height\":%d,\"frames_shown\":%llu,\"frames_decoded\":%llu,\"fps\":%.2f,"
                       "\"latency_ms\":{\"avg\":%.2f,\"p50\":%.2f,\"p95\":%.2f,\"p99\":%.2f,\"max\":%.2f,\"samples\":%zu},\"decode_avg_ms\":%.3f,\"recv_kbps\":%.1f,"
                       "\"inputs_sent\":%llu,\"audio_packets\":%llu,\"audio_samples\":%llu,\"audio_underruns\":%llu,\"keyframe_requests\":%llu,\"decode_skips\":%llu,\"decode_failures\":%llu}\n",
                    a.headless ? "true" : "false", gs.hw_dec ? "true" : "false", gs.w.load(), gs.h.load(), (unsigned long long)gs.frames_shown.load(), (unsigned long long)gs.frames_decoded.load(),
                    gs.frames_shown.load() / secs, avg, p50, p95, p99, mx, l2.size(), dec_avg, gs.bytes_rx.load() * 8.0 / 1000.0 / secs,
                    (unsigned long long)gs.inputs_tx.load(), (unsigned long long)gs.audio_rx.load(), (unsigned long long)gs.audio_samples.load(),
                    (unsigned long long)gs.audio_underrun.load(), (unsigned long long)gs.keys_req.load(), (unsigned long long)gs.decode_skips.load(), (unsigned long long)gs.decode_fail.load());
            fclose(f);
        }
    }
    if (adev) SDL_CloseAudioDevice(adev);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return gs.fatal_code.load();
}

} // namespace

int run_selftest(const Args& a)
{
    // local codec pipeline check with timings: pattern -> BGRA->NV12 -> H.264 encode -> decode, and tone -> AAC -> decode
    auto src = make_test_pattern(1280, 720, a.fps);
    H264Encoder enc; std::string err;
    if (!enc.init(1280, 720, a.fps, a.bitrate, !a.sw_enc, &err)) { fprintf(stderr, "enc: %s\n", err.c_str()); return 5; }
    H264Decoder dec; if (!dec.init(!a.sw_dec, &err)) { fprintf(stderr, "dec: %s\n", err.c_str()); return 5; }
    logf("selftest: encoder=%s hw=%d decoder_hw=%d", enc.name().c_str(), enc.hardware(), dec.hardware());
    double gen = 0, en = 0, de = 0, lat = 0; int n = 0, outs = 0, decs = 0;
    for (int i = 0; i < 120; i++) {
        BgraFrame f; uint64_t t0 = steady_us(); src->grab(&f, 100); uint64_t t1 = steady_us();
        std::vector<EncodedFrame> eo; enc.encode(f, i == 0, &eo); uint64_t t2 = steady_us();
        gen += (t1 - t0) / 1000.0; en += (t2 - t1) / 1000.0; n++; outs += (int)eo.size();
        for (auto& e : eo) {
            std::vector<DecodedFrame> d; uint64_t t3 = steady_us();
            dec.decode(e.data.data(), e.data.size(), e.cap_us, &d); de += (steady_us() - t3) / 1000.0;
            for (auto& x : d) { decs++; lat += (double)(uint32_t)(now32() - (uint32_t)x.cap_us) / 1000.0; }
        }
    }
    logf("selftest video: grab(+pattern gen/pace) %.2f ms, encode %.2f ms/frame, decode %.2f ms/frame, encoded=%d decoded=%d, capture->decoded %.1f ms", gen / n, en / n, outs ? de / outs : 0, outs, decs, decs ? lat / decs : 0);
    AacEncoder ae; AacDecoder ad; std::string e2;
    if (!ae.init(&e2)) logf("selftest audio: AAC enc init: %s", e2.c_str());
    else if (!ad.init(&e2)) logf("selftest audio: AAC dec init: %s", e2.c_str());
    else {
        auto tone = make_tone_source(); size_t frames = 0, samples = 0;
        for (int i = 0; i < 100; i++) {
            std::vector<int16_t> pcm; tone->read(&pcm, 20);
            std::vector<std::vector<uint8_t>> out; ae.encode(pcm.data(), pcm.size() / 2, &out);
            for (auto& f : out) { frames++; std::vector<int16_t> dp; ad.decode(f.data(), f.size(), &dp); samples += dp.size() / 2; }
        }
        logf("selftest audio: AAC frames=%zu decoded samples/ch=%zu", frames, samples);
    }
    return 0;
}

/* Windows.Graphics.Capture check against a window of our own that is 1/255 opaque and click-through (effectively invisible; off-screen windows are not composed by DWM so produce no frames). */
int run_selftest_wgc()
{
    HWND hw = nullptr; std::atomic<bool> stop{false}, ready{false};
    std::thread ui([&] {
        WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"rpl_wgc_test";
        RegisterClassW(&wc);
        hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"rpl_wgc_test", L"rpl wgc test", WS_POPUP, 200, 200, 800, 450, nullptr, nullptr, wc.hInstance, nullptr);
        SetLayeredWindowAttributes(hw, 0, 1, LWA_ALPHA);   // 1/255 opaque: invisible and click-through, but still composed by DWM
        ShowWindow(hw, SW_SHOWNOACTIVATE); ready = true;
        int n = 0;
        while (!stop) {
            MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            HDC dc = GetDC(hw);
            HBRUSH b = CreateSolidBrush(RGB((n * 3) & 255, 80, 200)); RECT r = {0, 0, 800, 450}; FillRect(dc, &r, b); DeleteObject(b);
            HBRUSH b2 = CreateSolidBrush(RGB(255, 255, 255)); RECT r2 = {(n * 7) % 700, 100, (n * 7) % 700 + 60, 160}; FillRect(dc, &r2, b2); DeleteObject(b2);
            ReleaseDC(hw, dc); n++; Sleep(8);
        }
        DestroyWindow(hw);
    });
    while (!ready) Sleep(10);
    Sleep(300);
    std::string err;
    auto cap = make_window_capture(hw, 60, &err);
    int rc = 1;
    if (!cap) logf("selftest-wgc: no capture: %s", err.c_str());
    else {
        logf("selftest-wgc: capture backend = %s %s", cap->name().c_str(), err.c_str());
        BgraFrame f, first; int n = 0, changed = 0; uint64_t t0 = steady_us(), lat = 0;
        while (steady_us() - t0 < 3000000) {
            if (!cap->grab(&f, 200)) continue;
            lat += steady_us() - f.cap_us;
            if (n == 0) first = f; else if (f.px.size() == first.px.size() && memcmp(f.px.data(), first.px.data(), f.px.size()) != 0) changed++;
            n++;
        }
        logf("selftest-wgc: %d frames in 3 s (%.1f fps), %dx%d, %d differ from the first frame", n, n / 3.0, f.w, f.h, changed);
        rc = (n > 20 && changed > 5) ? 0 : 1;
    }
    cap.reset();
    {   // WASAPI process loopback activation on our own PID (silent, but proves the API path works on this machine)
        std::string aerr; auto ac = make_process_loopback(GetCurrentProcessId(), &aerr);
        if (!ac) logf("selftest-wgc: process loopback: FAILED (%s)", aerr.c_str());
        else { std::vector<int16_t> pcm; for (int i = 0; i < 10; i++) ac->read(&pcm, 50); logf("selftest-wgc: process loopback activated OK (%zu samples read while silent)", pcm.size()); }
    }
    stop = true; ui.join();
    logf(rc == 0 ? "selftest-wgc: PASS" : "selftest-wgc: FAIL");
    return rc;
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--selftest-wgc") return run_selftest_wgc();
    Args a;
    if (!parse_args(argc, argv, &a)) { usage(); return 2; }
    timeBeginPeriod(1);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    if (a.selftest) return run_selftest(a);
    make_quit_event();
    if (a.probe) return run_probe(a);
    return a.host ? run_host(a) : run_guest(a);
}
