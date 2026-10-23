#include "identity_link.h"
#include "net_internal.h"
#include "session.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace hubnet {
using namespace detail;   // Json, json_parse, json_escape (net_internal.h)
namespace {

namespace fs = std::filesystem;

constexpr DWORD kPbkdf2Iters = 200000;
constexpr char kAad[] = "rh-link-v1";
constexpr size_t kSaltLen = 16, kIvLen = 12, kTagLen = 16;
constexpr char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

bool is_b32(const std::string& s, size_t len)
{
    if (s.size() != len) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    return true;
}

bool random_bytes(uint8_t* p, size_t n)
{
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string b64enc(const std::vector<uint8_t>& v)
{
    if (v.empty()) return "";
    DWORD n = 0;
    CryptBinaryToStringA(v.data(), (DWORD)v.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string s(n, '\0');
    CryptBinaryToStringA(v.data(), (DWORD)v.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &s[0], &n);
    s.resize(std::strlen(s.c_str()));
    return s;
}

bool b64dec(const std::string& s, std::vector<uint8_t>* out)
{
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return false;
    out->resize(n);
    if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, out->data(), &n, nullptr, nullptr)) return false;
    out->resize(n);
    return true;
}

bool derive_key(const std::string& code, const uint8_t* salt, uint8_t key[32])
{
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        return false;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(h, (PUCHAR)code.data(), (ULONG)code.size(), (PUCHAR)salt, (ULONG)kSaltLen,
                                        kPbkdf2Iters, key, 32, 0);
    BCryptCloseAlgorithmProvider(h, 0);
    return BCRYPT_SUCCESS(st);
}

/* AES-256-GCM over `data` in place. encrypt: data = plaintext in, ciphertext out, tag produced.
 * decrypt: data = ciphertext in, plaintext out, tag verified. */
bool aes_gcm(bool encrypt, const uint8_t key[32], const uint8_t iv[kIvLen], std::vector<uint8_t>& data, uint8_t tag[kTagLen])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hk = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                         sizeof(BCRYPT_CHAIN_MODE_GCM), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &hk, nullptr, 0, (PUCHAR)key, 32, 0))) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)iv;
        info.cbNonce = (ULONG)kIvLen;
        info.pbAuthData = (PUCHAR)kAad;
        info.cbAuthData = (ULONG)(sizeof kAad - 1);
        info.pbTag = tag;
        info.cbTag = (ULONG)kTagLen;
        ULONG outlen = 0;
        std::vector<uint8_t> out(data.size());
        NTSTATUS st = encrypt
            ? BCryptEncrypt(hk, data.data(), (ULONG)data.size(), &info, nullptr, 0, out.data(), (ULONG)out.size(), &outlen, 0)
            : BCryptDecrypt(hk, data.data(), (ULONG)data.size(), &info, nullptr, 0, out.data(), (ULONG)out.size(), &outlen, 0);
        if (BCRYPT_SUCCESS(st)) {
            out.resize(outlen);
            data = out;
            ok = true;
        }
    }
    if (hk) BCryptDestroyKey(hk);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string identity_to_json(const Identity& id)
{
    return "{\"user_id\":" + std::string("\"") + json::escape(id.user_id) + "\",\"secret\":\"" + json::escape(id.secret) +
           "\",\"created\":" + std::to_string(id.created) + "}\n";
}

bool identity_from_json(const std::string& text, Identity* out)
{
    Json j;
    if (!json_parse(text, &j) || j.type != Json::Object) return false;
    Identity t;
    t.user_id = j.str("user_id");
    t.secret = j.str("secret");
    const Json* c = j.get("created");
    t.created = (c && c->type == Json::Number) ? (int64_t)c->n : 0;
    if (!is_b32(t.user_id, 26) || !is_b32(t.secret, 52)) return false;
    *out = t;
    return true;
}

/* ---- HTTPS POST (WinHTTP), path like "/link/create" ---- */

struct HttpResult { int status = 0; std::string body; };

bool http_post(const std::string& server, const std::string& path, const std::string& body, HttpResult* r, std::string* err)
{
    std::string base = server.empty() ? default_server() : server;
    bool secure = true;
    for (const char* p : {"wss://", "https://"})
        if (base.rfind(p, 0) == 0) base = base.substr(std::strlen(p));
    for (const char* p : {"ws://", "http://"}) {
        if (base.rfind(p, 0) == 0) { base = base.substr(std::strlen(p)); secure = false; }
    }
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!secure) {
        /* Credentials must not travel in clear text: plain http/ws only for loopback or an explicit dev flag. */
        std::string h = base.substr(0, base.find(':'));
        bool local = h == "localhost" || h == "127.0.0.1" || h == "[::1]";
        const char* dev = std::getenv("RETROHUB_ALLOW_INSECURE_LINK");
        if (!local && !(dev && dev[0] == '1')) { *err = "refusing an unencrypted link server"; return false; }
    }
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = base.find(':');
    if (colon != std::string::npos) {
        try {
            int pv = std::stoi(base.substr(colon + 1));
            if (pv < 1 || pv > 65535) throw std::out_of_range("port");
            port = (INTERNET_PORT)pv;
        } catch (...) { *err = "invalid server address"; return false; }
        base = base.substr(0, colon);
    }
    std::wstring whost(base.begin(), base.end()), wpath(path.begin(), path.end());

    HINTERNET hs = WinHttpOpen(L"RetroHub/link", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { *err = "network unavailable"; return false; }
    WinHttpSetTimeouts(hs, 5000, 5000, 5000, 10000);
    HINTERNET hc = WinHttpConnect(hs, whost.c_str(), port, 0);
    HINTERNET hr = hc ? WinHttpOpenRequest(hc, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                      : nullptr;
    bool ok = hr && WinHttpSendRequest(hr, L"Content-Type: application/json\r\n", (DWORD)-1L,
                                        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(hr, nullptr);
    if (ok) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &sz, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)code;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hr, &avail) && avail) {
            std::vector<char> buf(avail);
            DWORD got = 0;
            if (!WinHttpReadData(hr, buf.data(), avail, &got)) break;
            r->body.append(buf.data(), got);
        }
    } else {
        *err = "could not reach the server";
    }
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    WinHttpCloseHandle(hs);
    return ok;
}

/* Server error text: the "error" field, or a generic message. */
std::string server_error(const HttpResult& r)
{
    Json j;
    std::string e = (json_parse(r.body, &j) && j.type == Json::Object) ? j.str("error") : "";
    return e.empty() ? ("server returned HTTP " + std::to_string(r.status)) : e;
}

} // namespace

std::string link_normalize_code(const std::string& code)
{
    std::string s;
    for (char c : code) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        s += (char)std::toupper((unsigned char)c);
    }
    if (s.size() != 8) return "";
    for (char c : s)
        if (!std::strchr(kAlphabet, c) || c == '\0') return "";
    return s;
}

bool link_seal(const std::string& code, const Identity& id, LinkEnvelope* out)
{
    std::string c = link_normalize_code(code);
    if (c.empty() || !is_b32(id.user_id, 26) || !is_b32(id.secret, 52)) return false;
    uint8_t salt[kSaltLen], iv[kIvLen], key[32], tag[kTagLen];
    if (!random_bytes(salt, sizeof salt) || !random_bytes(iv, sizeof iv) || !derive_key(c, salt, key)) return false;
    std::string plain = identity_to_json(id);
    std::vector<uint8_t> data(plain.begin(), plain.end());
    if (!aes_gcm(true, key, iv, data, tag)) return false;
    data.insert(data.end(), tag, tag + kTagLen);
    out->salt = b64enc(std::vector<uint8_t>(salt, salt + kSaltLen));
    out->iv = b64enc(std::vector<uint8_t>(iv, iv + kIvLen));
    out->ct = b64enc(data);
    return true;
}

bool link_open(const std::string& code, const LinkEnvelope& env, Identity* out)
{
    std::string c = link_normalize_code(code);
    std::vector<uint8_t> salt, iv, ct;
    if (c.empty() || !b64dec(env.salt, &salt) || !b64dec(env.iv, &iv) || !b64dec(env.ct, &ct)) return false;
    if (salt.size() != kSaltLen || iv.size() != kIvLen || ct.size() <= kTagLen) return false;
    uint8_t key[32], tag[kTagLen];
    if (!derive_key(c, salt.data(), key)) return false;
    std::memcpy(tag, ct.data() + ct.size() - kTagLen, kTagLen);
    std::vector<uint8_t> data(ct.begin(), ct.end() - kTagLen);
    if (!aes_gcm(false, key, iv.data(), data, tag)) return false;   // wrong code or tampering
    return identity_from_json(std::string(data.begin(), data.end()), out);
}

bool link_create_code(const std::string& server, const Identity& id, std::string* code_out, std::string* err)
{
    std::string auth = "\"uid\":\"" + id.user_id + "\",\"secret\":\"" + id.secret + "\"";
    HttpResult r;
    std::string e;
    if (!http_post(server, "/link/create", "{" + auth + "}", &r, &e)) { *err = e; return false; }
    Json j;
    if (r.status != 200 || !json_parse(r.body, &j)) { *err = "create failed: " + server_error(r); return false; }
    std::string code = j.str("code");
    if (link_normalize_code(code).empty()) { *err = "server returned a bad code"; return false; }

    LinkEnvelope env;
    if (!link_seal(code, id, &env)) { *err = "could not encrypt the identity"; return false; }
    HttpResult p;
    std::string body = "{" + auth + ",\"code\":\"" + code + "\",\"salt\":\"" + env.salt + "\",\"iv\":\"" + env.iv +
                       "\",\"ct\":\"" + env.ct + "\"}";
    if (!http_post(server, "/link/put", body, &p, &e)) { *err = e; return false; }
    if (p.status != 200) { *err = "upload failed: " + server_error(p); return false; }
    *code_out = code;
    return true;
}

bool link_redeem_code(const std::string& server, const std::string& code, Identity* out, std::string* err)
{
    std::string c = link_normalize_code(code);
    if (c.empty()) { *err = "Enter the 8-character code"; return false; }
    HttpResult r;
    std::string e;
    if (!http_post(server, "/link/redeem", "{\"code\":\"" + c + "\"}", &r, &e)) { *err = e; return false; }
    if (r.status == 409) { *err = "The other PC has not finished uploading yet. Try again in a moment."; return false; }
    if (r.status == 429) { *err = "Too many attempts. Wait a few minutes."; return false; }
    if (r.status == 404) { *err = "Code is wrong, expired or already used."; return false; }
    Json j;
    if (r.status != 200 || !json_parse(r.body, &j)) { *err = "redeem failed: " + server_error(r); return false; }
    LinkEnvelope env{j.str("salt"), j.str("iv"), j.str("ct")};
    if (!link_open(c, env, out)) { *err = "Could not decrypt the identity. Check the code and create a new one."; return false; }
    return true;
}

bool identity_export_file(const Identity& id, const std::string& path, std::string* err)
{
    if (!is_b32(id.user_id, 26) || !is_b32(id.secret, 52)) { *err = "no valid identity"; return false; }
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) { *err = "could not open the file for writing"; return false; }
    f << identity_to_json(id);
    if (!f) { *err = "write failed"; return false; }
    return true;
}

bool identity_import_file(const std::string& path, Identity* out, std::string* err)
{
    if (!identity_read_file(path, out)) { *err = "not a valid identity file"; return false; }
    return true;
}

} // namespace hubnet
