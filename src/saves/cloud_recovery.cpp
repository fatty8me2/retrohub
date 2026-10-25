/* Recovery code: the friends account (uid 26 + secret 52 base32 chars) as 20 groups of 4, last group = checksum.
 * The text is the account secret: callers must never log it. */
#include "cloud_internal.h"

#include <cctype>

namespace hub::saves {

namespace {
const char* kAlpha = "abcdefghijklmnopqrstuvwxyz234567";
int val(char c) {
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return 26 + (c - '2');
    return -1;
}
std::string check(const std::string& s) {   // 2 chars = 10 bits
    unsigned sum = 0;
    for (size_t i = 0; i < s.size(); i++) sum += (unsigned)(i + 1) * (unsigned)val(s[i]);
    sum %= 1024;
    std::string o;
    o += kAlpha[sum >> 5];
    o += kAlpha[sum & 31];
    return o;
}
}  // namespace

std::string recovery_encode(const std::string& uid, const std::string& secret) {
    if (uid.size() != 26 || secret.size() != 52) return std::string();
    std::string raw = uid + secret;
    for (char c : raw) if (val(c) < 0) return std::string();
    raw += check(raw);
    std::string out;
    for (size_t i = 0; i < raw.size(); i += 4) {
        if (i) out += '-';
        for (size_t j = i; j < i + 4 && j < raw.size(); j++) out += (char)toupper((unsigned char)raw[j]);
    }
    return out;
}

bool recovery_decode(const std::string& code, std::string* uid, std::string* secret, std::string* err) {
    std::string raw;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (val(c) < 0) { if (err) *err = "The recovery code has a character that is not allowed."; return false; }
        raw += (char)tolower((unsigned char)c);
    }
    if (raw.size() != 80) { if (err) *err = "The recovery code should have 80 letters and digits."; return false; }
    if (check(raw.substr(0, 78)) != raw.substr(78)) { if (err) *err = "The recovery code does not match (check for a typo)."; return false; }
    if (uid) *uid = raw.substr(0, 26);
    if (secret) *secret = raw.substr(26, 52);
    return true;
}

}  // namespace hub::saves
