// Console test for invite links: parsing, link building, protocol command, and the two-process
// single-instance handoff. Uses unique mutex and pipe names, never touches the registry, opens no windows.
// Usage: hub_invite_test            (runs everything)
//        hub_invite_test --send <pipe> <code>   (child mode: the second launch)
#include "invite.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::wstring self_exe() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf, n);
}

static DWORD run_child(const std::wstring& args) {
    std::wstring cmd = L"\"" + self_exe() + L"\" " + args;
    std::vector<wchar_t> mut(cmd.begin(), cmd.end());
    mut.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 999;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 998;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static void parsing_tests() {
    std::wstring c;
    check(invite_is_valid_code(L"ABCDEF"), "valid code");
    check(invite_is_valid_code(L"2345NP"), "digits and N/P accepted");
    check(!invite_is_valid_code(L"ABCDE"), "5 chars rejected");
    check(!invite_is_valid_code(L"ABCDE0"), "zero rejected");
    check(!invite_is_valid_code(L"ABCDEI"), "I rejected");
    check(!invite_is_valid_code(L"ABCDEO"), "O rejected");
    check(!invite_is_valid_code(L"ABC DE"), "space rejected");
    check(invite_normalize_code(L"  abcdef \t") == L"ABCDEF", "normalize trims and uppercases");

    check(invite_parse_url(L"retrohub://join/ABCDEF", &c) && c == L"ABCDEF", "protocol url parses");
    check(invite_parse_url(L"RETROHUB://JOIN/abcdef/", &c) && c == L"ABCDEF", "scheme case-insensitive, trailing slash ok");
    check(!invite_parse_url(L"retrohub://join/ABCDE", &c), "short code url rejected");
    check(!invite_parse_url(L"retrohub://join/ABCDEF/extra", &c), "extra path rejected");
    check(!invite_parse_url(L"http://join/ABCDEF", &c), "other scheme rejected");
    check(!invite_parse_url(L"retrohub://open/ABCDEF", &c), "other retrohub action rejected");

    check(invite_parse_command_line(L"\"C:\\x\\RetroHub.exe\" --join ABCDEF", &c) && c == L"ABCDEF", "--join <CODE> parses");
    check(invite_parse_command_line(L"RetroHub.exe \"retrohub://join/ABCDEF\"", &c) && c == L"ABCDEF", "quoted protocol arg parses");
    check(!invite_parse_command_line(L"RetroHub.exe --join BAD0CODE", &c), "--join with bad code rejected");
    check(!invite_parse_command_line(L"RetroHub.exe --join", &c), "--join without value rejected");
    check(!invite_parse_command_line(L"RetroHub.exe", &c), "no join args: not a join");
}

static void link_tests() {
    check(invite_web_link(L"abcdef") == L"https://smash-netplay.jumpcircuit.workers.dev/j/ABCDEF", "web invite link");
    check(invite_web_link(L"ABCDE0").empty(), "invalid code gives empty link");
    check(invite_protocol_command(L"C:\\Apps\\RetroHub.exe") == L"\"C:\\Apps\\RetroHub.exe\" \"%1\"", "protocol command");
}

static void single_instance_tests() {
    DWORD id = GetCurrentProcessId();
    std::wstring mutex = L"Local\\RetroHubTest.Mutex." + std::to_wstring(id);
    std::wstring pipe = L"\\\\.\\pipe\\RetroHubTest.Pipe." + std::to_wstring(id);

    InviteInstance first(mutex);
    check(first.is_primary(), "first holder is primary");
    InviteInstance second(mutex);
    check(!second.is_primary(), "second holder is not primary");

    InviteServer server;
    check(server.start(pipe), "primary pipe server starts");

    DWORD rc = run_child(L"--send \"" + pipe + L"\" ABCDEF");
    check(rc == 0, "second process hands ABCDEF to the primary (exit 0)");
    std::wstring got;
    bool queued = server.take(&got);
    check(queued && got == L"ABCDEF", "primary queued the code from the other process");
    check(!server.take(&got), "queue is empty after one take");

    rc = run_child(L"--send \"" + pipe + L"\" BAD0CODE");
    check(rc != 0, "invalid code is refused (nonzero exit)");
    check(!server.take(&got), "invalid code is not queued");

    server.stop();
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--send") {
        return invite_send(argv[2], argv[3], 3000) ? 0 : 1;
    }
    parsing_tests();
    link_tests();
    single_instance_tests();
    std::printf("%s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
