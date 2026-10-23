#pragma once
/* Clickable invite links for RetroHub.
 *
 *   retrohub://join/<CODE>      protocol link (registered by shell_ensure_url_protocol, see shell_integration.h)
 *   RetroHub.exe --join <CODE>  command-line form
 *   https://smash-netplay.jumpcircuit.workers.dev/j/<CODE>   web landing page (the copyable invite link)
 *
 * Parsing and link building are pure. The single-instance handoff uses a named mutex (who is primary) and a
 * named pipe (a second launch sends its code to the primary). Nothing here creates a window. */
#include <string>

#include <windows.h>

/* Room-code alphabet matches the Worker's CODE_RE (server/cloudflare/src/index.js): 6 chars, no 0/1/I/O. */
bool invite_is_valid_code(const std::wstring& code);
std::wstring invite_normalize_code(const std::wstring& raw);   // trims and upper-cases

/* retrohub://join/<CODE> (scheme case-insensitive, one optional trailing '/'). */
bool invite_parse_url(const std::wstring& url, std::wstring* code);
/* Scans a full command line (GetCommandLineW) for "--join <CODE>" or a retrohub:// URL argument. */
bool invite_parse_command_line(const std::wstring& cmdline, std::wstring* code);

/* https://smash-netplay.jumpcircuit.workers.dev/j/<CODE>. Returns "" for an invalid code. */
std::wstring invite_web_link(const std::wstring& code);
/* Puts text on the clipboard. Returns false on failure. */
bool invite_copy_text(const std::wstring& text);

/* Registry command for the protocol handler: "<exe>" "%1". */
std::wstring invite_protocol_command(const std::wstring& exe);

/* Single-instance handoff. Production names: see the constants below. */
extern const wchar_t* const INVITE_MUTEX_NAME;   // Local\RetroHub.SingleInstance
extern const wchar_t* const INVITE_PIPE_NAME;    // \\.\pipe\RetroHub.Invite

class InviteInstance {   // holds the named mutex; is_primary() is true for the first holder
public:
    explicit InviteInstance(const std::wstring& mutex_name);
    ~InviteInstance();
    InviteInstance(const InviteInstance&) = delete;
    InviteInstance& operator=(const InviteInstance&) = delete;
    bool is_primary() const { return primary_; }
private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
};

class InviteServer {   // primary side: accepts codes on the pipe and queues the valid ones
public:
    InviteServer() = default;
    ~InviteServer();
    bool start(const std::wstring& pipe_name);
    void stop();
    bool take(std::wstring* code);   // pops the next queued code (main thread, once per frame)
private:
    void run();
    std::wstring pipe_;
    HANDLE thread_ = nullptr;
    volatile LONG stop_ = 0;
    CRITICAL_SECTION lock_{};
    bool lock_ready_ = false;
    std::wstring queue_[16];
    int count_ = 0;
};

/* Second-launch side: sends a code to the primary. Retries until timeout_ms while the primary starts up.
 * Returns true when the primary accepted the code. */
bool invite_send(const std::wstring& pipe_name, const std::wstring& code, DWORD timeout_ms = 3000);
