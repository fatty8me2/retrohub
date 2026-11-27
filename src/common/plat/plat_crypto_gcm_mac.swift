// AES-GCM through Apple CryptoKit (CommonCrypto has no public GCM). Minimal C-callable shim for plat_crypto.cpp.
// Status: 0 ok, 1 bad argument, 2 authentication / crypto failure.
import CryptoKit
import Foundation

private func bytes(_ p: UnsafeRawPointer?, _ n: Int) -> Data {
    guard let p = p, n > 0 else { return Data() }
    return Data(bytes: p, count: n)
}

@_cdecl("rh_aes_gcm_seal")
public func rh_aes_gcm_seal(
    _ key: UnsafeRawPointer?, _ keyLen: Int,
    _ nonce: UnsafeRawPointer?, _ nonceLen: Int,
    _ aad: UnsafeRawPointer?, _ aadLen: Int,
    _ plain: UnsafeRawPointer?, _ plainLen: Int,
    _ cipherOut: UnsafeMutableRawPointer?,   // plainLen bytes
    _ tagOut: UnsafeMutableRawPointer?       // 16 bytes
) -> Int32 {
    guard keyLen == 16 || keyLen == 24 || keyLen == 32, nonceLen == 12, key != nil, nonce != nil,
          aadLen >= 0, plainLen >= 0, tagOut != nil, plainLen == 0 || cipherOut != nil else { return 1 }
    do {
        let n = try AES.GCM.Nonce(data: bytes(nonce, nonceLen))
        let box = try AES.GCM.seal(bytes(plain, plainLen), using: SymmetricKey(data: bytes(key, keyLen)),
                                   nonce: n, authenticating: bytes(aad, aadLen))
        guard box.ciphertext.count == plainLen, box.tag.count == 16 else { return 2 }
        if plainLen > 0 { box.ciphertext.copyBytes(to: cipherOut!.assumingMemoryBound(to: UInt8.self), count: plainLen) }
        box.tag.copyBytes(to: tagOut!.assumingMemoryBound(to: UInt8.self), count: 16)
        return 0
    } catch { return 2 }
}

@_cdecl("rh_aes_gcm_open")
public func rh_aes_gcm_open(
    _ key: UnsafeRawPointer?, _ keyLen: Int,
    _ nonce: UnsafeRawPointer?, _ nonceLen: Int,
    _ aad: UnsafeRawPointer?, _ aadLen: Int,
    _ cipher: UnsafeRawPointer?, _ cipherLen: Int,
    _ tag: UnsafeRawPointer?, _ tagLen: Int,
    _ plainOut: UnsafeMutableRawPointer?     // cipherLen bytes
) -> Int32 {
    guard keyLen == 16 || keyLen == 24 || keyLen == 32, nonceLen == 12, tagLen == 16, key != nil, nonce != nil,
          tag != nil, aadLen >= 0, cipherLen >= 0, cipherLen == 0 || (cipher != nil && plainOut != nil) else { return 1 }
    do {
        let n = try AES.GCM.Nonce(data: bytes(nonce, nonceLen))
        let box = try AES.GCM.SealedBox(nonce: n, ciphertext: bytes(cipher, cipherLen), tag: bytes(tag, tagLen))
        let plain = try AES.GCM.open(box, using: SymmetricKey(data: bytes(key, keyLen)), authenticating: bytes(aad, aadLen))
        guard plain.count == cipherLen else { return 2 }
        if cipherLen > 0 { plain.copyBytes(to: plainOut!.assumingMemoryBound(to: UInt8.self), count: cipherLen) }
        return 0
    } catch { return 2 }
}
