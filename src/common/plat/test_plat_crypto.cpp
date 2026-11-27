/* plat_crypto: known-answer vectors (FIPS 180, RFC 1321, RFC 4231), streaming == one-shot, random bytes. */
#include "plat_crypto.h"

#include <algorithm>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

#if defined(__APPLE__)
static std::string unhex(const char* h) {
    std::string o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) o += (char)std::stoi(std::string(h + i, 2), nullptr, 16);
    return o;
}

static void test_gcm() {
    /* McGrew-Viega GCM spec, Test Case 16 (also NIST SP 800-38D example set): AES-256, 96-bit IV, 20-byte AAD. */
    std::string K = unhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    std::string IV = unhex("cafebabefacedbaddecaf888");
    std::string P = unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39");
    std::string A = unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    std::string C, T, back;
    CHECK(plat::aes_gcm_seal(K, IV, A, P, &C, &T));
    CHECK(plat::hex(C) == "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662");
    CHECK(plat::hex(T) == "76fc6ece0f4e1768cddf8853bb2d551b");
    CHECK(plat::aes_gcm_open(K, IV, A, C, T, &back) && back == P);

    /* tamper rejection; plain stays empty */
    std::string bad = C;
    bad[5] ^= 0x01;
    back = "x";
    CHECK(!plat::aes_gcm_open(K, IV, A, bad, T, &back) && back.empty());
    std::string badt = T;
    badt[15] ^= 0x80;
    CHECK(!plat::aes_gcm_open(K, IV, A, C, badt, &back) && back.empty());
    std::string bada = A;
    bada[0] ^= 0x01;
    CHECK(!plat::aes_gcm_open(K, IV, bada, C, T, &back) && back.empty());
    CHECK(!plat::aes_gcm_open(K, IV, "", C, T, &back));
    CHECK(!plat::aes_gcm_open(K, unhex("cafebabefacedbaddecaf889"), A, C, T, &back));

    /* round trip: empty plaintext, empty AAD, 16- and 32-byte keys */
    for (size_t kl : {16, 32}) {
        std::string k(kl, 'k'), c2, t2, p2;
        CHECK(plat::aes_gcm_seal(k, IV, "", "", &c2, &t2) && c2.empty() && t2.size() == 16);
        CHECK(plat::aes_gcm_open(k, IV, "", c2, t2, &p2) && p2.empty());
    }
    CHECK(!plat::aes_gcm_seal("short", IV, "", "x", &C, &T));   // bad key length
    CHECK(!plat::aes_gcm_seal(K, "123", "", "x", &C, &T));      // bad nonce length

    /* Envelope sealed by the old hand-written CTR+GHASH code in identity_link.cpp (key = 00..1f, iv = a0..ab,
     * AAD "rh-link-v1"): the CryptoKit path must open it byte-for-byte. */
    std::string k2, iv2;
    for (int i = 0; i < 32; ++i) k2 += (char)i;
    for (int i = 0; i < 12; ++i) iv2 += (char)(0xa0 + i);
    std::string oldCt = unhex("9d3a095e20b95dd60647bdf1551fb4ac1fe42c72b2de2609f27a4ff2068b1968bc1d678bca51271f73be6aea334eb184672b777a51e42f48766632");
    std::string oldTag = unhex("5f3f23eab5ee1cdbdc4e6d15a2bfa25e");
    std::string pt;
    CHECK(plat::aes_gcm_open(k2, iv2, "rh-link-v1", oldCt, oldTag, &pt));
    CHECK(pt == "{\"user_id\":\"RetroHub identity link test\",\"n\":42} 0123456789");
}
#endif


int main() {
    using plat::HashAlg;
    CHECK(plat::hex(plat::hash(HashAlg::Md5, "abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(plat::hex(plat::hash(HashAlg::Sha1, "abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(plat::hex(plat::hash(HashAlg::Sha256, "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(plat::hex(plat::hash(HashAlg::Sha512, "abc")) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(plat::hex(plat::hash(HashAlg::Sha256, "")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    /* streaming in odd-sized pieces equals one-shot */
    std::string big;
    for (int i = 0; i < 100000; ++i) big += (char)(i * 31 + 7);
    plat::Hasher h(HashAlg::Sha256);
    CHECK(h.ok());
    for (size_t off = 0; off < big.size(); off += 977) h.update(big.data() + off, std::min<size_t>(977, big.size() - off));
    CHECK(h.finish() == plat::hash(HashAlg::Sha256, big));

    /* RFC 4231 test case 2 */
    CHECK(plat::hex(plat::hmac(HashAlg::Sha256, "Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

#if defined(__APPLE__)
    test_gcm();
#endif

    unsigned char a[32] = {}, b[32] = {};
    CHECK(plat::random_bytes(a, sizeof a) && plat::random_bytes(b, sizeof b));
    CHECK(std::string((char*)a, 32) != std::string((char*)b, 32));

    std::printf(g_fail ? "plat_crypto_test: %d FAILED\n" : "plat_crypto_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
