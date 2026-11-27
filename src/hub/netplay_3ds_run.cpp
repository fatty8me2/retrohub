#include "winutil.h"
#include "netplay_3ds_run.h"

#include "plat/plat_sock.h"
#if RH_WIN
#include <windows.h>
#endif

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#if RH_WIN
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#else
#include "plat/plat_proc.h"
#include "plat/plat_wake.h"
#endif

#include "session.h"

namespace hubnp3ds {

/* GameCube: a guest may still be unpacking/hashing a disc or starting Dolphin; Dolphin NetPlay holds a long game, so be patient. */
static const uint32_t kGcReadyMs = 90000, kGcSilenceMs = 30000;

#if RH_WIN
static std::wstring W(const std::string& s) { return winutil::to_wide(s); }
#else
using SOCKET = plat::socket_t;
static const SOCKET INVALID_SOCKET = plat::kBadSocket;
static inline int closesocket(SOCKET s) { return plat::sock_close(s); }
#endif

std::string room_exe_for(const std::string& azahar_exe) {
#if !RH_WIN
    // TODO(macOS lane 7): Azahar ships as a .app; azahar-room is expected beside the azahar executable (Contents/MacOS).
    std::error_code ec;
    std::filesystem::path r = std::filesystem::path(azahar_exe).parent_path() / "azahar-room";
    return std::filesystem::exists(r, ec) ? r.string() : "";
#else
    size_t p = azahar_exe.find_last_of("\\/");
    if (p == std::string::npos) return "";
    std::string r = azahar_exe.substr(0, p + 1) + "azahar-room.exe";
    return GetFileAttributesW(W(r).c_str()) != INVALID_FILE_ATTRIBUTES ? r : "";
#endif
}

struct Runner::Impl {
    bool host = false, active = false, failed = false, wsa = false;
    Plan plan;
    std::string status = "idle";
    hubnet::Session sess;
#if RH_WIN
    HANDLE job = nullptr, proc = nullptr, stdin_w = nullptr;
#else
    plat::proc::Child room;   // azahar-room (own process group)
#endif
    std::map<int, SOCKET> host_sock;   // guest slot -> socket talking to azahar-room
    SOCKET guest_sock = INVALID_SOCKET;
    sockaddr_in guest_client{};
    bool have_client = false;
    int local_port = 0;
    uint16_t next_id = 1;
    Reassembler re;
    unsigned long long out = 0, in = 0;
    uint32_t t0 = 0;
    std::string tag = "3ds";
    /* GameCube: Dolphin's NetPlay traffic is forwarded on its own thread (forward_loop), not on the ~60 Hz UI tick, so a packet waits
     * ~1 ms instead of up to a frame on each PC. mu guards everything above while the thread runs. */
    std::recursive_mutex mu;
    std::thread th;
    std::atomic<bool> quit{false};
    bool threaded = false;
    std::string room_id(const std::string& code, int launch_no) const { return tag == "3ds" ? session_room(code, launch_no) : code + "-" + std::to_string(launch_no); }   // other tags: the Worker only accepts CODE-N game rooms

    static SOCKET udp(int port, int* bound) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return s;
        plat::sock_set_nonblocking(s, true);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)port);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
        plat::socklen len = sizeof a;
        getsockname(s, (sockaddr*)&a, &len);
        if (bound) *bound = ntohs(a.sin_port);
        return s;
    }
    sockaddr_in room_addr() const {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)plan.port);
        return a;
    }
    void send_out(int to_slot, const char* d, int n) {
        for (auto& f : fragment(next_id++, (const uint8_t*)d, (size_t)n)) sess.send(to_slot, f.data(), (int)f.size());
        ++out;
    }
    /* One event wakes the thread for anything to forward: Dolphin's local sockets and the Session's UDP socket (WSAEventSelect FD_READ)
     * and queued relay frames (Session::set_wake_event). Waking on data does not depend on the timer resolution (a 1 ms select/Sleep
     * timeout becomes ~16 ms when Windows does not grant 1 ms timers, measured: 30 ms loopback round trip). */
#if RH_WIN
    WSAEVENT wake = WSA_INVALID_EVENT;
    void watch(SOCKET s) { if (wake != WSA_INVALID_EVENT && s != INVALID_SOCKET) WSAEventSelect(s, wake, FD_READ); }
#else
    /* macOS: a plat::Waker (signalled by Session for relay frames) plus poll() on the local sockets. hubnet::Session has no public
     * accessor for its UDP socket fd (src/net is not ours), so direct UDP frames cannot wake the poll: the wait is capped at 1 ms
     * (up to ~1 ms extra latency on direct frames; relay frames and local Dolphin/Azahar datagrams still wake it at once).
     * TODO(macOS lane 2): expose Session::udp_fd() and add it to the poll set, then raise the cap to 5 ms like Windows. */
    plat::Waker* wake = nullptr;
    void watch(SOCKET) {}
#endif
    void step();
    void forward_loop() {
#if !RH_WIN
        while (!quit.load()) {
            { std::lock_guard<std::recursive_mutex> g(mu); step(); }
            std::vector<int> fds;
            { std::lock_guard<std::recursive_mutex> g(mu);
              if (guest_sock != INVALID_SOCKET) fds.push_back((int)guest_sock);
              for (auto& kv : host_sock) fds.push_back((int)kv.second); }
            plat::wait_readable(*wake, fds, 1);
        }
        return;
#endif
#if RH_WIN
        timeBeginPeriod(1);
        while (!quit.load()) {
            WSAResetEvent(wake);   // before draining: anything arriving after this sets it again
            { std::lock_guard<std::recursive_mutex> g(mu); step(); }
            WaitForSingleObject(wake, 5);   // data wakes it at once; the timeout only paces keepalives / timeouts
        }
        timeEndPeriod(1);
#endif
    }
    void start_thread() {
#if RH_WIN
        wake = WSACreateEvent();
        if (wake == WSA_INVALID_EVENT) return;   // no event: stay on the UI-loop pump rather than spin
        threaded = true;
        watch(guest_sock);
        for (auto& kv : host_sock) watch(kv.second);
        sess.set_wake_event(wake);
#else
        wake = new plat::Waker();
        if (!wake->ok()) { delete wake; wake = nullptr; return; }
        threaded = true;
        sess.set_wake_event(wake);
#endif
        th = std::thread([this] { forward_loop(); });
    }
    void pump() {
        char buf[2048];
        if (host) {
            for (auto& kv : host_sock) {
                for (;;) {
                    int n = recvfrom(kv.second, buf, sizeof buf, 0, nullptr, nullptr);
                    if (n <= 0) break;
                    send_out(kv.first, buf, n);
                }
            }
        } else if (guest_sock != INVALID_SOCKET) {
            for (;;) {
                sockaddr_in from{};
                plat::socklen fl = sizeof from;
                int n = recvfrom(guest_sock, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
                if (n <= 0) break;
                guest_client = from;
                have_client = true;
                send_out(0, buf, n);
            }
        }
        int from_slot;
        std::vector<uint8_t> d;
        while (sess.recv(&from_slot, &d)) {
            std::string whole;
            if (!re.add(from_slot, d.data(), d.size(), hubnet::now_ms(), &whole)) continue;
            ++in;
            if (host) {
                auto it = host_sock.find(from_slot);
                if (it == host_sock.end()) {
                    SOCKET s = udp(0, nullptr);
                    if (s == INVALID_SOCKET) continue;
                    watch(s);
                    it = host_sock.emplace(from_slot, s).first;
                }
                sockaddr_in ra = room_addr();
                sendto(it->second, whole.data(), (int)whole.size(), 0, (sockaddr*)&ra, sizeof ra);
            } else if (have_client && from_slot == 0) {
                sendto(guest_sock, whole.data(), (int)whole.size(), 0, (sockaddr*)&guest_client, sizeof guest_client);
            }
        }
    }
};

Runner::Runner() : impl_(new Impl) {}
Runner::~Runner() { stop(); }

static bool wsa_init(Runner*, bool* flag) {
    if (*flag) return true;
#if RH_WIN
    WSADATA w;
    *flag = WSAStartup(MAKEWORD(2, 2), &w) == 0;
#else
    *flag = plat::sock_init();
#endif
    return *flag;
}

bool Runner::start_host(const std::string& server, const std::string& code, int launch_no, const Plan& plan,
                        const std::string& room_exe, const std::string& work_dir, std::string* err) {
    stop();
    Impl& m = *impl_;
    if (room_exe.empty()) { if (err) *err = "azahar-room.exe not found next to azahar.exe (update the Azahar install)"; return false; }
    if (!wsa_init(this, &m.wsa)) { if (err) *err = "winsock unavailable"; return false; }
    m.host = true;
    m.plan = plan;
#if RH_WIN
    /* kill-on-close job so the room never outlives RetroHub; a held stdin pipe keeps the room from seeing EOF */
    m.job = CreateJobObjectW(nullptr, nullptr);
    if (m.job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m.job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    CreatePipe(&r, &w, &sa, 0);
    SetHandleInformation(w, HANDLE_FLAG_INHERIT, 0);
    /* the room prints member joins/leaves to stdout: keep it (QA evidence), next to its --log-file */
    HANDLE nul = CreateFileW(W(work_dir + "\\azahar-room.out").c_str(), GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = r;
    si.hStdOutput = nul;
    si.hStdError = nul;
    std::string args = room_args(plan.port, plan.password, kMaxMembers, work_dir + "\\azahar-room.log", "");
    std::wstring cmd = L"\"" + W(room_exe) + L"\" " + W(args);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(W(room_exe).c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                             W(work_dir).c_str(), &si, &pi);
    CloseHandle(r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(w); if (err) *err = "could not start azahar-room.exe"; return false; }
    if (m.job) AssignProcessToJobObject(m.job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    m.proc = pi.hProcess;
    m.stdin_w = w;
#else
    {
        /* own process group: stop() kill_tree()s it. TODO(macOS lane 6a/7): plat::proc::spawn gives the child no held stdin pipe;
         * if azahar-room exits on stdin EOF there, add a stdin option to SpawnOptions. */
        plat::proc::SpawnOptions o;
        o.exe = room_exe;
        o.args = {"--port", std::to_string(plan.port), "--room-name", "RetroHub", "--max_members", std::to_string(kMaxMembers),
                  "--preferred-app", "RetroHub", "--preferred-app-id", "0"};   // same switches as room_args()
        if (!plan.password.empty()) { o.args.push_back("--password"); o.args.push_back(plan.password); }
        o.args.push_back("--log-file");
        o.args.push_back(plat::join(work_dir, "azahar-room.log"));
        o.cwd = work_dir;
        o.stdout_path = o.stderr_path = plat::join(work_dir, "azahar-room.out");
        o.activate = false;
        std::string se;
        if (!plat::proc::spawn(o, &m.room, &se)) { if (err) *err = "could not start azahar-room: " + se; return false; }
    }
#endif
    hubnet::SessionConfig c;
    c.server = server;
    c.room = m.room_id(code, launch_no);
    c.slot = 0;
    c.expected = plan.expected;
    if (!m.sess.start(c)) { if (err) *err = "could not start the session"; stop(); return false; }
    m.active = true;
    m.t0 = hubnet::now_ms();
    m.status = "Waiting for everyone to connect...";
    return true;
}

bool Runner::start_host_forward(const std::string& server, const std::string& code, int launch_no, const Plan& plan,
                                const std::string& tag, std::string* err) {
    stop();
    Impl& m = *impl_;
    if (!wsa_init(this, &m.wsa)) { if (err) *err = "winsock unavailable"; return false; }
    m.host = true;
    m.plan = plan;
    m.tag = tag;
    hubnet::SessionConfig c;
    c.server = server;
    c.room = m.room_id(code, launch_no);
    c.slot = 0;
    c.expected = plan.expected;
    if (tag == "gc") { c.ready_timeout_ms = kGcReadyMs; c.silence_fail_ms = kGcSilenceMs; }
    if (!m.sess.start(c)) { if (err) *err = "could not start the session"; stop(); return false; }
    m.active = true;
    m.t0 = hubnet::now_ms();
    m.status = "Waiting for everyone to connect...";
    if (m.tag == "gc" && !std::getenv("RETROHUB_GCFW_NOTHREAD")) m.start_thread();
    return true;
}

bool Runner::start_guest(const std::string& server, const std::string& code, int launch_no, const Plan& plan, int my_slot,
                         std::string* err, const std::string& tag) {
    stop();
    Impl& m = *impl_;
    if (!wsa_init(this, &m.wsa)) { if (err) *err = "winsock unavailable"; return false; }
    m.host = false;
    m.plan = plan;
    m.tag = tag;
    m.guest_sock = Impl::udp(0, &m.local_port);
    if (m.guest_sock == INVALID_SOCKET) { if (err) *err = "no free local UDP port"; return false; }
    hubnet::SessionConfig c;
    c.server = server;
    c.room = m.room_id(code, launch_no);
    c.slot = my_slot;
    c.expected = plan.expected;
    if (tag == "gc") { c.ready_timeout_ms = kGcReadyMs; c.silence_fail_ms = kGcSilenceMs; }
    if (!m.sess.start(c)) { if (err) *err = "could not start the session"; stop(); return false; }
    m.active = true;
    m.t0 = hubnet::now_ms();
    m.status = "Connecting to the host...";
    if (m.tag == "gc" && !std::getenv("RETROHUB_GCFW_NOTHREAD")) m.start_thread();
    return true;
}

void Runner::update() {
    Impl& m = *impl_;
    if (!m.active || m.threaded) return;   // GameCube: the forwarder thread steps the session
    m.step();
}

void Runner::Impl::step() {
    Impl& m = *this;
    if (!m.active) return;
    m.sess.update();
    m.pump();
    if (m.sess.failed()) { m.failed = true; m.status = "Connection failed: " + m.sess.status(); return; }
#if RH_WIN
    if (m.host && m.proc && WaitForSingleObject(m.proc, 0) == WAIT_OBJECT_0) { m.failed = true; m.status = "The room server exited (port in use?)"; return; }
#else
    if (m.host && m.room.valid() && !plat::proc::running(m.room)) { m.failed = true; m.status = "The room server exited (port in use?)"; return; }
#endif
    if (!m.sess.ready()) {
        uint32_t lim = m.tag == "gc" ? kGcReadyMs + 10000 : 25000;   // the session fails first (with the reason); this is the backstop
        m.status = hubnet::now_ms() - m.t0 > lim ? "Timed out connecting to the other players" : "Connecting: " + m.sess.status();
        if (hubnet::now_ms() - m.t0 > lim) m.failed = true;
    } else m.status = "Connected";
}

void Runner::stop() {
    Impl& m = *impl_;
#if RH_WIN
    if (m.th.joinable()) { m.quit = true; SetEvent(m.wake); m.th.join(); }   // wakes the loop at once
#else
    if (m.th.joinable()) { m.quit = true; if (m.wake) m.wake->signal(); m.th.join(); }
#endif
    m.threaded = false;
    m.quit = false;
    m.sess.stop();   // joins the WS thread first: it may still SetEvent the wake handle
#if RH_WIN
    if (m.wake != WSA_INVALID_EVENT) { m.sess.set_wake_event(nullptr); WSACloseEvent(m.wake); m.wake = WSA_INVALID_EVENT; }
#else
    if (m.wake) { m.sess.set_wake_event(nullptr); delete m.wake; m.wake = nullptr; }
#endif
    for (auto& kv : m.host_sock) closesocket(kv.second);
    m.host_sock.clear();
    if (m.guest_sock != INVALID_SOCKET) closesocket(m.guest_sock);
    m.guest_sock = INVALID_SOCKET;
#if RH_WIN
    if (m.stdin_w) { CloseHandle(m.stdin_w); m.stdin_w = nullptr; }
    if (m.proc) { TerminateProcess(m.proc, 0); CloseHandle(m.proc); m.proc = nullptr; }
    if (m.job) { CloseHandle(m.job); m.job = nullptr; }
#else
    if (m.room.valid()) { plat::proc::kill_tree(m.room); m.room = plat::proc::Child{}; }
#endif
    m.active = m.failed = m.have_client = false;
    m.out = m.in = 0;
    m.status = "idle";
}

bool Runner::active() const { return impl_->active; }
bool Runner::ready() const { std::lock_guard<std::recursive_mutex> g(impl_->mu); return impl_->active && !impl_->failed && impl_->sess.ready(); }
int Runner::worst_ping_ms() const {
    std::lock_guard<std::recursive_mutex> g(impl_->mu);
    int w = -1;
    if (!impl_->active) return w;
    for (int i = 0; i < hubnet::kMaxSlots; ++i) { int p = impl_->sess.ping_ms(i); if (p > w) w = p; }
    return w;
}
bool Runner::any_relayed() const {
    std::lock_guard<std::recursive_mutex> g(impl_->mu);
    if (!impl_->active) return false;
    for (int i = 0; i < hubnet::kMaxSlots; ++i) if (impl_->sess.is_relayed(i)) return true;
    return false;
}
bool Runner::failed() const { std::lock_guard<std::recursive_mutex> g(impl_->mu); return impl_->failed; }
std::string Runner::status() const { std::lock_guard<std::recursive_mutex> g(impl_->mu); return impl_->status; }
int Runner::connect_port() const { return impl_->host ? impl_->plan.port : impl_->local_port; }
unsigned long long Runner::datagrams_out() const { std::lock_guard<std::recursive_mutex> g(impl_->mu); return impl_->out; }
unsigned long long Runner::datagrams_in() const { std::lock_guard<std::recursive_mutex> g(impl_->mu); return impl_->in; }

}  // namespace hubnp3ds
