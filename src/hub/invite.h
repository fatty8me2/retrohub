#pragma once
/* Clickable invite links for RetroHub.
 *
 *   retrohub://join/<CODE>      protocol link (registered by shell_ensure_url_protocol, see shell_integration.h)
 *   RetroHub.exe --join <CODE>  command-line form
 *   https://smash-netplay.jumpcircuit.workers.dev/j/<CODE>   web landing page (the copyable invite link)
 *
 * Parsing and link building are pure. The single-instance handoff uses a named mutex (who is primary) and a
 * named pipe (a second launch sends its code to the primary). Nothing here creates a window. */
#include <functional>
#include <memory>
#include <string>

#include <windows.h>

/* Room-code alphabet matches the Worker's CODE_RE (server/cloudflare/src/index.js): 6 chars, no 0/1/I/O. */
bool invite_is_valid_code(const std::wstring& code);
std::wstring invite_normalize_code(const std::wstring& raw);   // trims and upper-cases

/* retrohub://join/<CODE> (scheme case-insensitive, one optional trailing '/'). */
bool invite_parse_url(const std::wstring& url, std::wstring* code);
/* Scans a full command line (GetCommandLineW) for "--join <CODE>" or a retrohub:// URL argument. */
bool invite_parse_command_line(const std::wstring& cmdline, std::wstring* code);

/* Account invite codes (owner-made, XXXX-XXXX; see net/invite_logic.h): retrohub://invite/<CODE> or "--invite <CODE>". Returns the code as "ABCD-EFGH". */
bool invite_parse_account_url(const std::wstring& url, std::wstring* code);
bool invite_parse_account_command_line(const std::wstring& cmdline, std::wstring* code);
bool invite_send_account(const std::wstring& pipe_name, const std::wstring& code, DWORD timeout_ms = 3000);

/* Game night reminder click: retrohub://night/<id>, id = "<uid>@<start seconds>" (see gamenight_logic.h make_id). Strictly validated:
 * uid = 1..60 of [A-Za-z0-9_.-], '@', 1..11 digits. Anything else is refused (never used as a path or command). */
bool invite_is_valid_night_id(const std::wstring& id);
bool invite_parse_night_url(const std::wstring& url, std::wstring* id);
bool invite_parse_night_command_line(const std::wstring& cmdline, std::wstring* id);
bool invite_send_night(const std::wstring& pipe_name, const std::wstring& id, DWORD timeout_ms = 3000);

/* One-click favorites: "RetroHub.exe --play <game key>" (library key: lowercase [a-z0-9._-], at most 120 chars). */
bool invite_is_valid_game_key(const std::wstring& key);
bool invite_parse_play(const std::wstring& cmdline, std::wstring* key);
/* Arguments of a game shortcut: --play <key>. */
std::wstring invite_play_arguments(const std::wstring& key);

/* https://smash-netplay.jumpcircuit.workers.dev/j/<CODE>. Returns "" for an invalid code. */
/* Optional host name (shown in the page title and chat previews as "Join <name>'s RetroHub room"). */
std::wstring invite_web_link(const std::wstring& code, const std::wstring& host_name = L"");
/* "?h=<url-encoded sanitized name>", or "" when nothing usable is left. */
std::wstring invite_host_query(const std::wstring& host_name);
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
    bool take(std::wstring* code);        // pops the next queued join code (main thread, once per frame)
    bool take_play(std::wstring* key);    // pops the next queued --play game key
    bool take_account(std::wstring* code);   // pops the next queued account invite code ("ABCD-EFGH")
    bool take_night(std::wstring* id);    // pops the next queued game night id (retrohub://night/<id>)
    bool take_show();                     // true once per "show yourself" request
private:
    bool pop(int kind, std::wstring* text);
    void push(int kind, const std::wstring& text);
    void run();
    std::wstring pipe_;
    HANDLE thread_ = nullptr;
    volatile LONG stop_ = 0;
    CRITICAL_SECTION lock_{};
    bool lock_ready_ = false;
    struct Msg { int kind; std::wstring text; } queue_[16];
    int count_ = 0;
};

/* Second-launch side: sends a code to the primary. Retries until timeout_ms while the primary starts up.
 * Returns true when the primary accepted the code. */
bool invite_send(const std::wstring& pipe_name, const std::wstring& code, DWORD timeout_ms = 3000);
bool invite_send_play(const std::wstring& pipe_name, const std::wstring& key, DWORD timeout_ms = 3000);
bool invite_send_show(const std::wstring& pipe_name, DWORD timeout_ms = 3000);

/* A second launch whose hand-over got no answer. The usual cause is a RetroHub that is still closing: its pipe server is already
 * stopped, but the process (whose handle keeps the mutex alive) has not ended yet. Running on beside it would leave this launch
 * non-primary for good (no pipe server: every later invite link or --play opens yet another hub) while both write the same files.
 * So: drop our handle, wait for that process to end and open the mutex again; true when *inst is the primary now. hand_over is retried
 * about every 2 s in case the other one is only slow: when it takes the request, *handed = true and the result is false. After
 * budget_ms: false (the caller carries on as before). */
bool invite_wait_for_primary(std::unique_ptr<InviteInstance>* inst, const std::wstring& mutex_name,
                             const std::function<bool(DWORD timeout_ms)>& hand_over, DWORD budget_ms, bool* handed);
