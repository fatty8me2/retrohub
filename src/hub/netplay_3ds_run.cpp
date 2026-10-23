#include "winutil.h"
#include "netplay_3ds_run.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <map>

#include "session.h"

namespace hubnp3ds {

static std::wstring W(const std::string& s) { return winutil::to_wide(s); }

std::string room_exe_for(const std::string& azahar_exe) {
    size_t p = azahar_exe.find_last_of("\\/");
    if (p == std::string::npos) return "";
    std::string r = azahar_exe.substr(0, p + 1) + "azahar-room.exe";
    return GetFileAttributesW(W(r).c_str()) != INVALID_FILE_ATTRIBUTES ? r : "";
}

struct Runner::Impl {
    bool host = false, active = false, failed = false, wsa = false;
    Plan plan;
    std::string status = "idle";
    hubnet::Session sess;
    HANDLE job = nullptr, proc = nullptr, stdin_w = nullptr;
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
    std::string room_id(const std::string& code, int launch_no) const { return tag == "3ds" ? session_room(code, launch_no) : code + "-" + std::to_string(launch_no); }   // other tags: the Worker only accepts CODE-N game rooms

    static SOCKET udp(int port, int* bound) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return s;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((u_short)port);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
        int len = sizeof a;
        getsockname(s, (sockaddr*)&a, &len);
        if (bound) *bound = ntohs(a.sin_port);
        return s;
    }
    sockaddr_in room_addr() const {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((u_short)plan.port);
        return a;
    }
    void send_out(int to_slot, const char* d, int n) {
        for (auto& f : fragment(next_id++, (const uint8_t*)d, (size_t)n)) sess.send(to_slot, f.data(), (int)f.size());
        ++out;
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
                int fl = sizeof from;
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
    WSADATA w;
    *flag = WSAStartup(MAKEWORD(2, 2), &w) == 0;
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
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
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
    if (!m.sess.start(c)) { if (err) *err = "could not start the session"; stop(); return false; }
    m.active = true;
    m.t0 = hubnet::now_ms();
    m.status = "Waiting for everyone to connect...";
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
    if (!m.sess.start(c)) { if (err) *err = "could not start the session"; stop(); return false; }
    m.active = true;
    m.t0 = hubnet::now_ms();
    m.status = "Connecting to the host...";
    return true;
}

void Runner::update() {
    Impl& m = *impl_;
    if (!m.active) return;
    m.sess.update();
    m.pump();
    if (m.sess.failed()) { m.failed = true; m.status = "Connection failed: " + m.sess.status(); return; }
    if (m.host && m.proc && WaitForSingleObject(m.proc, 0) == WAIT_OBJECT_0) { m.failed = true; m.status = "The room server exited (port in use?)"; return; }
    if (!m.sess.ready()) {
        m.status = hubnet::now_ms() - m.t0 > 25000 ? "Timed out connecting to the other players" : "Connecting: " + m.sess.status();
        if (hubnet::now_ms() - m.t0 > 25000) m.failed = true;
    } else m.status = "Connected";
}

void Runner::stop() {
    Impl& m = *impl_;
    m.sess.stop();
    for (auto& kv : m.host_sock) closesocket(kv.second);
    m.host_sock.clear();
    if (m.guest_sock != INVALID_SOCKET) closesocket(m.guest_sock);
    m.guest_sock = INVALID_SOCKET;
    if (m.stdin_w) { CloseHandle(m.stdin_w); m.stdin_w = nullptr; }
    if (m.proc) { TerminateProcess(m.proc, 0); CloseHandle(m.proc); m.proc = nullptr; }
    if (m.job) { CloseHandle(m.job); m.job = nullptr; }
    m.active = m.failed = m.have_client = false;
    m.out = m.in = 0;
    m.status = "idle";
}

bool Runner::active() const { return impl_->active; }
bool Runner::ready() const { return impl_->active && !impl_->failed && impl_->sess.ready(); }
bool Runner::failed() const { return impl_->failed; }
std::string Runner::status() const { return impl_->status; }
int Runner::connect_port() const { return impl_->host ? impl_->plan.port : impl_->local_port; }
unsigned long long Runner::datagrams_out() const { return impl_->out; }
unsigned long long Runner::datagrams_in() const { return impl_->in; }

}  // namespace hubnp3ds
