/* macOS ECDSA P-256 verification for the updater (updater.cpp, hubupdate::detail::verify_ecdsa_p256). Windows uses BCrypt with the
 * key as raw X||Y and the signature as raw r||s; here Security.framework takes the key as an ANSI X9.63 point (0x04 || X || Y) and the
 * signature as the DER SEQUENCE the release signer writes (openssl dgst -sha256 -sign), over the whole message (SHA-256 is done inside
 * kSecKeyAlgorithmECDSASignatureMessageX962SHA256). The strict DER shape check stays in updater.cpp (der_to_raw_rs), so both
 * platforms accept exactly the same signatures. */
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <cstddef>
#include <cstring>

namespace hubupdate {
bool hubupdate_mac_ecdsa_verify(const unsigned char pub[64], const void* msg, size_t msgLen, const void* sigDer, size_t sigLen) {
    if (!pub || (!msg && msgLen) || !sigDer || !sigLen) return false;
    bool ok = false;
    @autoreleasepool {
        unsigned char point[65];
        point[0] = 0x04;
        std::memcpy(point + 1, pub, 64);
        NSData* keyData = [NSData dataWithBytes:point length:sizeof point];
        NSDictionary* attrs = @{
            (__bridge id)kSecAttrKeyType: (__bridge id)kSecAttrKeyTypeECSECPrimeRandom,
            (__bridge id)kSecAttrKeyClass: (__bridge id)kSecAttrKeyClassPublic,
            (__bridge id)kSecAttrKeySizeInBits: @256,
        };
        CFErrorRef err = nullptr;
        SecKeyRef key = SecKeyCreateWithData((__bridge CFDataRef)keyData, (__bridge CFDictionaryRef)attrs, &err);
        if (err) { CFRelease(err); err = nullptr; }
        if (key) {
            NSData* message = [NSData dataWithBytes:msg length:msgLen];
            NSData* signature = [NSData dataWithBytes:sigDer length:sigLen];
            const SecKeyAlgorithm alg = kSecKeyAlgorithmECDSASignatureMessageX962SHA256;
            if (SecKeyIsAlgorithmSupported(key, kSecKeyOperationTypeVerify, alg)) {
                ok = SecKeyVerifySignature(key, alg, (__bridge CFDataRef)message, (__bridge CFDataRef)signature, &err) == true;
                if (err) CFRelease(err);
            }
            CFRelease(key);
        }
    }
    return ok;
}
}  // namespace hubupdate
