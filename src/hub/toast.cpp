#include "toast.h"
#include "popup_guard.h"

namespace hubtoast {

std::string xml_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: if (c >= 0x20 || c == '\t') o += (char)c; break;   // drop control characters
        }
    }
    return o;
}

static bool code_ok(const std::string& c) {   // 6 chars, no 0/1/I/O (same alphabet as the Worker)
    if (c.size() != 6) return false;
    for (char ch : c) {
        bool up = (ch >= 'A' && ch <= 'Z' && ch != 'I' && ch != 'O');
        bool dg = (ch >= '2' && ch <= '9');
        if (!up && !dg) return false;
    }
    return true;
}

std::string build_invite_xml(const std::string& friend_name, const std::string& game_title, const std::string& code) {
    if (!code_ok(code)) return "";
    std::string who = friend_name.empty() ? "A friend" : friend_name;
    std::string game = game_title.empty() ? "a game" : game_title;
    std::string url = "retrohub://join/" + code;
    return "<toast launch=\"" + url + "\" activationType=\"protocol\" scenario=\"default\">"
           "<visual><binding template=\"ToastGeneric\">"
           "<text>" + xml_escape(who) + " invited you to " + xml_escape(game) + "</text>"
           "<text>Join after your current game ends.</text>"
           "</binding></visual><audio silent=\"true\"/>"
           "<actions><action content=\"Join\" arguments=\"" + url + "\" activationType=\"protocol\"/></actions>"
           "</toast>";
}

std::string build_online_xml(const std::string& friend_name) {
    std::string who = friend_name.empty() ? "A friend" : friend_name;
    return "<toast scenario=\"default\"><visual><binding template=\"ToastGeneric\">"
           "<text>" + xml_escape(who) + " came online</text>"
           "</binding></visual><audio silent=\"true\"/></toast>";
}

std::string build_gamenight_xml(const std::string& game_title, const std::string& host_name, const std::string& night_id) {
    std::string id;   // the id is "<uid>@<seconds>": keep the activation link to plain characters
    for (char c : night_id) if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '@' || c == '-' || c == '_' || c == '.') id += c;
    std::string url = "retrohub://night/" + id;
    std::string game = game_title.empty() ? "a session" : game_title;
    std::string line = host_name.empty() ? "Open RetroHub to host or join." : "With " + host_name + ". Open RetroHub to join.";
    return "<toast launch=\"" + xml_escape(url) + "\" activationType=\"protocol\" scenario=\"default\">"
           "<visual><binding template=\"ToastGeneric\">"
           "<text>Game night in 10 minutes: " + xml_escape(game) + "</text>"
           "<text>" + xml_escape(line) + "</text>"
           "</binding></visual><audio silent=\"true\"/></toast>";
}

bool parse_activation_code(const std::string& args, std::string* code) {
    static const char kPre[] = "retrohub://join/";
    const size_t n = sizeof(kPre) - 1;
    if (args.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = args[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != kPre[i]) return false;
    }
    std::string rest = args.substr(n);
    if (!rest.empty() && rest.back() == '/') rest.pop_back();
    for (auto& c : rest) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (!code_ok(rest)) return false;
    if (code) *code = rest;
    return true;
}

bool should_toast(bool setting_on, bool game_running, bool notifier_available) {
    /* A toast only ever meant "tell the player who is in a game", and the house rule forbids exactly that: the guard says no whenever a
     * game runs, so this is false for every input today. The bell and the runner OSD carry the message instead. */
    return setting_on && notifier_available && game_running && hubguard::may_windows_toast(game_running);
}

bool notify_invite(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name,
                   const std::string& game_title, const std::string& code) {
    if (!should_toast(setting_on, game_running, n.available())) return false;
    std::string xml = build_invite_xml(friend_name, game_title, code);
    if (xml.empty()) return false;
    return n.show(xml, code);
}

bool notify_online(INotifier& n, bool setting_on, bool game_running, const std::string& friend_name) {
    if (!should_toast(setting_on, game_running, n.available())) return false;
    return n.show(build_online_xml(friend_name), "online");
}

}  // namespace hubtoast
