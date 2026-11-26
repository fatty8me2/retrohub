#include "plat_crypto.h"

#include "plat.h"

#if RH_WIN
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <Security/SecRandom.h>
#endif

namespace plat {

size_t hash_size(HashAlg a) {
    switch (a) {
    case HashAlg::Md5: return 16;
    case HashAlg::Sha1: return 20;
    case HashAlg::Sha256: return 32;
    case HashAlg::Sha512: return 64;
    }
    return 0;
}

#if RH_WIN

static const wchar_t* alg_id(HashAlg a) {
    switch (a) {
    case HashAlg::Md5: return BCRYPT_MD5_ALGORITHM;
    case HashAlg::Sha1: return BCRYPT_SHA1_ALGORITHM;
    case HashAlg::Sha256: return BCRYPT_SHA256_ALGORITHM;
    case HashAlg::Sha512: return BCRYPT_SHA512_ALGORITHM;
    }
    return BCRYPT_SHA256_ALGORITHM;
}

struct Hasher::Impl {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    size_t n = 0;
    bool ok = false;
    Impl(HashAlg a, const std::string* key) : n(hash_size(a)) {
        if (BCryptOpenAlgorithmProvider(&alg, alg_id(a), nullptr, key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0) { alg = nullptr; return; }
        ok = BCryptCreateHash(alg, &h, nullptr, 0, key ? (PUCHAR)key->data() : nullptr, key ? (ULONG)key->size() : 0, 0) == 0;
        if (!ok) h = nullptr;
    }
    ~Impl() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void update(const void* d, size_t len) {
        const unsigned char* p = (const unsigned char*)d;
        while (ok && len) {   // BCryptHashData takes a ULONG length
            const ULONG chunk = len > 0x40000000u ? 0x40000000u : (ULONG)len;
            ok = BCryptHashData(h, (PUCHAR)p, chunk, 0) == 0;
            p += chunk;
            len -= chunk;
        }
    }
    std::string finish() {
        if (!ok) return std::string();
        std::string out(n, '\0');
        ok = BCryptFinishHash(h, (PUCHAR)&out[0], (ULONG)n, 0) == 0;
        return ok ? out : std::string();
    }
};

bool random_bytes(void* buf, size_t len) {
    return BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

#else

struct Hasher::Impl {
    HashAlg a;
    bool ok = true, is_hmac = false;
    CC_MD5_CTX md5;
    CC_SHA1_CTX sha1;
    CC_SHA256_CTX sha256;
    CC_SHA512_CTX sha512;
    CCHmacContext hm;
    Impl(HashAlg alg, const std::string* key) : a(alg) {
        if (key) {
            is_hmac = true;
            CCHmacAlgorithm ca = kCCHmacAlgSHA256;
            switch (a) {
            case HashAlg::Md5: ca = kCCHmacAlgMD5; break;
            case HashAlg::Sha1: ca = kCCHmacAlgSHA1; break;
            case HashAlg::Sha256: ca = kCCHmacAlgSHA256; break;
            case HashAlg::Sha512: ca = kCCHmacAlgSHA512; break;
            }
            CCHmacInit(&hm, ca, key->data(), key->size());
            return;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // MD5 / SHA-1 are deprecated for security use; RetroHub uses them for file ids
        switch (a) {
        case HashAlg::Md5: CC_MD5_Init(&md5); break;
        case HashAlg::Sha1: CC_SHA1_Init(&sha1); break;
        case HashAlg::Sha256: CC_SHA256_Init(&sha256); break;
        case HashAlg::Sha512: CC_SHA512_Init(&sha512); break;
        }
#pragma clang diagnostic pop
    }
    void update(const void* d, size_t len) {
        const unsigned char* p = (const unsigned char*)d;
        while (len) {   // CC_*_Update take a CC_LONG (32-bit) length
            const CC_LONG chunk = len > 0x40000000u ? 0x40000000u : (CC_LONG)len;
            if (is_hmac) CCHmacUpdate(&hm, p, chunk);
            else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                switch (a) {
                case HashAlg::Md5: CC_MD5_Update(&md5, p, chunk); break;
                case HashAlg::Sha1: CC_SHA1_Update(&sha1, p, chunk); break;
                case HashAlg::Sha256: CC_SHA256_Update(&sha256, p, chunk); break;
                case HashAlg::Sha512: CC_SHA512_Update(&sha512, p, chunk); break;
                }
#pragma clang diagnostic pop
            }
            p += chunk;
            len -= chunk;
        }
    }
    std::string finish() {
        std::string out(hash_size(a), '\0');
        unsigned char* o = (unsigned char*)&out[0];
        if (is_hmac) { CCHmacFinal(&hm, o); return out; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        switch (a) {
        case HashAlg::Md5: CC_MD5_Final(o, &md5); break;
        case HashAlg::Sha1: CC_SHA1_Final(o, &sha1); break;
        case HashAlg::Sha256: CC_SHA256_Final(o, &sha256); break;
        case HashAlg::Sha512: CC_SHA512_Final(o, &sha512); break;
        }
#pragma clang diagnostic pop
        return out;
    }
};

bool random_bytes(void* buf, size_t len) { return SecRandomCopyBytes(kSecRandomDefault, len, buf) == errSecSuccess; }

#endif

Hasher::Hasher(HashAlg a) : p_(new Impl(a, nullptr)) {}
Hasher::~Hasher() = default;
bool Hasher::ok() const { return p_->ok; }
void Hasher::update(const void* data, size_t len) { p_->update(data, len); }
std::string Hasher::finish() { return p_->finish(); }

std::string hash(HashAlg a, const void* data, size_t len) {
    Hasher h(a);
    h.update(data, len);
    return h.finish();
}

std::string hmac(HashAlg a, const std::string& key, const void* data, size_t len) {
    Hasher::Impl i(a, &key);
    i.update(data, len);
    return i.finish();
}

std::string hex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(raw.size() * 2);
    for (unsigned char c : raw) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

} // namespace plat
