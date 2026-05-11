// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include "ST87M01Modem.h"

// TLS profile / credential management for the ST87M01.
//
// Wraps the AT#TLSCERT* and AT#TLSKEY* command families. A "security
// profile" is a numeric slot (1-9) on the modem that holds a CA root
// cert, optionally a device cert + private key (mutual TLS), and PSK
// material (Phase 3). Once provisioned, the profile id is passed to
// ST87M01HTTP::begin(..., secProfile=...) to upgrade an HTTP socket to
// HTTPS.
//
// Supports:
//   - Server-authenticated TLS: import a CA root cert.
//   - Mutual TLS (mTLS): import a CA root cert + client certificate +
//     ECC P-256 private key into the same profile. The modem validates
//     key-certificate correspondence at import time.
//   - On-modem key generation and CSR signing are planned for Phase 3.
//
// Persistence: AT#TLSCERTADD / AT#TLSKEYADD take effect immediately but
// live in RAM until AT#RESET=1 commits to NVM. Provision once, then call
// saveToNvm() — the modem reboots and network registration must be redone.
//
// Important caveats. Some come straight from ST's TLS Application Note
// (v2.0, 2025-07-30); others were discovered on hardware (firmware as of
// 2026-05-04) and aren't in the doc.
//
// Documented:
//   * Certificate chain validation is NOT supported. The modem only
//     verifies that the server's leaf certificate is signed by exactly
//     the cert you imported — it will not walk a chain. For typical
//     deployments (server → intermediate → root) you must import the
//     INTERMEDIATE that directly issues the leaf, not the root. (TLS
//     app note §3.3.2.)
//   * Only DER input is supported by AT#TLSCERTADD. The Pem overloads
//     in this class decode to DER in the host before upload.
//   * The TLS 1.2 cipher suite list is exclusively ECDHE_ECDSA + PSK.
//     There are no RSA cipher suites in 1.2. TLS 1.3 suites are
//     auth-agnostic by name, but in practice the parser below kicks in
//     before any handshake. (TLS app note Table 6.)
//   * Private keys must be ECC SECP R1 (P-256) or BrainpoolP256, 32
//     bytes. The modem internally derives the public key from the
//     imported private key. (AT manual §14.5.)
//   * The modem validates key-certificate correspondence when a client
//     cert is imported — a mismatch returns an error. (TLS app note
//     §4.3 / §4.4.)
//
// Hardware-only (not in the doc):
//   * The CA-cert parser is ECDSA-only and curve-restricted to P-256 /
//     Brainpool-P256. RSA roots and ECDSA P-384 roots are rejected
//     with a bare "+CME ERROR" at AT#TLSCERTADD time, before any
//     handshake.
//   * AT#TLSCERTADD and AT#TLSKEYADD require AT+CFUN=4 (RF off). With
//     RF active the modem returns a bare "+CME ERROR". This class
//     auto-toggles AT+CFUN=4 around each provisioning call and restores
//     the prior CFUN level on the way out.
//   * Practical CA-size limit: cert + framing must fit in ~1.5 KB on
//     the AT line (hex-encoded, so ~750 bytes DER). ECDSA P-256 roots
//     comfortably fit; RSA-4096 (e.g. ISRG Root X1) is too big and the
//     command silently hangs.
//   * Most public Let's Encrypt sites can NOT be talked to — LE chains
//     are either ISRG Root X1 (RSA-4096) or X2 (P-384), neither
//     acceptable. Use AWS IoT, an ECC CloudFront distribution, or
//     self-hosted P-256 chains. And remember: import the intermediate
//     that signs the leaf, not the root.
//
// Server-auth usage:
//     ST87M01TLS tls(modem);
//     tls.addCaCertPem(1, caCertPem);
//     tls.saveToNvm();        // optional: persist across power cycle
//     ST87M01HTTP https(modem);
//     https.begin("api.example.com", 443, /*secProfile=*/1);
//
// Mutual TLS usage:
//     ST87M01TLS tls(modem);
//     tls.addCaCertPem(1, caCertPem);
//     tls.addPrivateKeyDer(1, eccP256Key, 32);
//     tls.addClientCertPem(1, clientCertPem);
//     tls.saveToNvm();
//     ST87M01HTTP https(modem);
//     https.begin("iot.example.com", 8443, /*secProfile=*/1);
class ST87M01TLS {
public:
  // Profile ids 0 is reserved (means "no security"); valid range is 1..9.
  static constexpr uint8_t MAX_PROFILE_ID = 9;

  // AT#TLSCERT* <type> values.
  enum CertType : uint8_t {
    CERT_DEVICE  = 0,    // device / client cert (mutual TLS)
    CERT_CA_ROOT = 1,    // CA root cert (server auth)
    CERT_PSK_ID  = 2,    // PSK identity (PSK suites)
  };

  struct CertInfo {
    uint8_t  profileId;
    uint8_t  type;          // CertType
    String   issuer;        // CN of the issuing party, e.g. "Amazon Root CA 3"
    String   subject;       // CN of the subject (== issuer for self-signed roots)
    String   notBefore;     // YYMMDDhhmmssZ, e.g. "150526000000Z"
    String   notAfter;      // YYMMDDhhmmssZ, e.g. "400526000000Z"
  };

  explicit ST87M01TLS(ST87M01Modem& modem);

  // ---- CA root cert (server auth) ------------------------------------

  // Add a CA root certificate to the named profile. The DER form takes the
  // raw X.509 bytes; the PEM forms accept the standard "-----BEGIN
  // CERTIFICATE----- ..." text and decode to DER internally. Effective
  // immediately; call saveToNvm() to persist across power cycle. profileId
  // must be 1-9 (0 is rejected — that value means "no security" elsewhere
  // in the library).
  bool addCaCertDer(uint8_t profileId, const uint8_t* der, size_t len);
  bool addCaCertPem(uint8_t profileId, const char* pem);
  bool addCaCertPem(uint8_t profileId, const char* pem, size_t pemLen);

  // ---- Client cert + private key (mutual TLS) -----------------------

  // Add a client (device) certificate. Same DER/PEM conventions as CA
  // certs. The modem validates that the client cert matches a previously
  // imported private key in the same profile — import the key first, then
  // the cert.
  bool addClientCertDer(uint8_t profileId, const uint8_t* der, size_t len);
  bool addClientCertPem(uint8_t profileId, const char* pem);
  bool addClientCertPem(uint8_t profileId, const char* pem, size_t pemLen);

  // Import an ECC SECP R1 (P-256) private key. The raw 32-byte scalar is
  // the only accepted format (not DER-wrapped). The modem stores it in
  // Flash, internally derives the public key, and will present it during
  // the TLS handshake when the server requests client authentication.
  bool addPrivateKey(uint8_t profileId, const uint8_t* key, size_t len);

  // Import an ECC P-256 private key from a PEM-encoded PKCS#8 or SEC1
  // (EC PRIVATE KEY) block. Extracts the raw 32-byte scalar and calls
  // addPrivateKey().
  bool addPrivateKeyPem(uint8_t profileId, const char* pem);
  bool addPrivateKeyPem(uint8_t profileId, const char* pem, size_t pemLen);

  // Remove the private key from a profile.
  bool deleteKey(uint8_t profileId);

  struct KeyInfo {
    uint8_t  profileId;
    uint8_t  keyType;     // 0 = private key, 1 = PSK
    uint8_t  curve;       // 0 = SECP_R1, 1 = BrainpoolP256
    uint16_t bits;        // key size in bits (256 for P-256)
  };

  // Enumerate provisioned keys. profileId=0 lists across every profile.
  bool listKeys(uint8_t profileId, KeyInfo* out, size_t maxItems, size_t& outCount);

  // ---- Removal / management -----------------------------------------

  // Remove a single credential or wipe every credential in the profile.
  // Note: the modem reports these as "takes effect after reboot" — call
  // saveToNvm() afterwards if you need the deletion to apply right now or
  // across power cycle.
  bool deleteCert(uint8_t profileId, uint8_t type);
  bool deleteProfile(uint8_t profileId);

  // Enumerate provisioned credentials. profileId=0 lists across every
  // profile (matches AT#TLSCERTLIST with omitted sec_id). Writes up to
  // maxItems entries into out and stores the actual count in outCount.
  bool listCerts(uint8_t profileId, CertInfo* out, size_t maxItems, size_t& outCount);

  // Commit pending TLS provisioning to non-volatile storage by issuing
  // AT#RESET=1. Reboots the modem; the caller must re-attach to the
  // network afterwards. Blocks up to bootMs waiting for the modem to come
  // back online.
  bool saveToNvm(unsigned long bootMs = 20000);

private:
  ST87M01Modem& _modem;

  bool uploadDer(uint8_t profileId, uint8_t type, const uint8_t* der, size_t len);
  bool uploadKey(uint8_t profileId, const uint8_t* key, size_t len);

  // Decode a PEM block (base64 between BEGIN/END markers, ignoring
  // whitespace) into DER bytes. Returns the number of DER bytes written,
  // or 0 on malformed input or insufficient buffer.
  static size_t pemToDer(const char* pem, size_t pemLen, uint8_t* out, size_t outMax);

  // Extract the raw 32-byte ECC P-256 private key scalar from a
  // DER-encoded PKCS#8 (SEQUENCE > SEQUENCE > OID > OCTET STRING >
  // SEQUENCE > INTEGER) or SEC1 (EC PRIVATE KEY: SEQUENCE > INTEGER >
  // OCTET STRING) structure. Returns 32 on success, 0 on failure.
  static size_t extractEcKeyFromDer(const uint8_t* der, size_t derLen,
                                    uint8_t* out, size_t outMax);
};
