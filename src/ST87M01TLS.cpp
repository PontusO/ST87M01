// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01TLS.h"

ST87M01TLS::ST87M01TLS(ST87M01Modem& modem) : _modem(modem) {}

namespace {

// RAII-ish helper: switch the modem to AT+CFUN=4 (RF off / minimum
// functionality) for the duration of a TLS provisioning command, then
// restore whatever level it was at on exit. The ST87M0 firmware rejects
// AT#TLSCERT* with a bare "+CME ERROR" if RF is active, so this is the
// difference between "works every time" and "always fails."
struct CfunGuard {
  ST87M01Modem& modem;
  uint8_t prior;
  bool toggled;
  bool ok;

  explicit CfunGuard(ST87M01Modem& m) : modem(m), prior(1), toggled(false), ok(true) {
    if (!modem.getFunctionality(prior)) prior = 1;  // safe default
    if (prior != 4) {
      modem.at().sendf("AT+CFUN=4");
      ok = modem.at().expectOK(10000);
      toggled = ok;
    }
  }

  ~CfunGuard() {
    if (toggled) {
      modem.at().sendf("AT+CFUN=%u", static_cast<unsigned>(prior));
      modem.at().expectOK(10000);
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// CA cert provisioning
// ---------------------------------------------------------------------------
bool ST87M01TLS::addCaCertDer(uint8_t profileId, const uint8_t* der, size_t len) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!der || !len) return false;
  return uploadDer(profileId, CERT_CA_ROOT, der, len);
}

bool ST87M01TLS::addCaCertPem(uint8_t profileId, const char* pem) {
  if (!pem) return false;
  return addCaCertPem(profileId, pem, strlen(pem));
}

bool ST87M01TLS::addCaCertPem(uint8_t profileId, const char* pem, size_t pemLen) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!pem || !pemLen) return false;

  uint8_t* der = static_cast<uint8_t*>(malloc(pemLen));
  if (!der) return false;

  size_t derLen = pemToDer(pem, pemLen, der, pemLen);
  bool ok = (derLen > 0) && uploadDer(profileId, CERT_CA_ROOT, der, derLen);
  free(der);
  return ok;
}

// ---------------------------------------------------------------------------
// Client cert provisioning (mutual TLS)
// ---------------------------------------------------------------------------
bool ST87M01TLS::addClientCertDer(uint8_t profileId, const uint8_t* der, size_t len) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!der || !len) return false;
  return uploadDer(profileId, CERT_DEVICE, der, len);
}

bool ST87M01TLS::addClientCertPem(uint8_t profileId, const char* pem) {
  if (!pem) return false;
  return addClientCertPem(profileId, pem, strlen(pem));
}

bool ST87M01TLS::addClientCertPem(uint8_t profileId, const char* pem, size_t pemLen) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!pem || !pemLen) return false;

  uint8_t* der = static_cast<uint8_t*>(malloc(pemLen));
  if (!der) return false;

  size_t derLen = pemToDer(pem, pemLen, der, pemLen);
  bool ok = (derLen > 0) && uploadDer(profileId, CERT_DEVICE, der, derLen);
  free(der);
  return ok;
}

// ---------------------------------------------------------------------------
// Private key provisioning (mutual TLS)
// ---------------------------------------------------------------------------
bool ST87M01TLS::addPrivateKey(uint8_t profileId, const uint8_t* key, size_t len) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!key || len != 32) return false;
  return uploadKey(profileId, key, len);
}

bool ST87M01TLS::addPrivateKeyPem(uint8_t profileId, const char* pem) {
  if (!pem) return false;
  return addPrivateKeyPem(profileId, pem, strlen(pem));
}

bool ST87M01TLS::addPrivateKeyPem(uint8_t profileId, const char* pem, size_t pemLen) {
  if (profileId > MAX_PROFILE_ID) return false;
  if (!pem || !pemLen) return false;

  uint8_t* der = static_cast<uint8_t*>(malloc(pemLen));
  if (!der) return false;

  size_t derLen = pemToDer(pem, pemLen, der, pemLen);
  if (derLen == 0) { free(der); return false; }

  uint8_t raw[32];
  size_t rawLen = extractEcKeyFromDer(der, derLen, raw, sizeof(raw));
  free(der);

  if (rawLen != 32) return false;
  return uploadKey(profileId, raw, rawLen);
}

bool ST87M01TLS::uploadDer(uint8_t profileId, uint8_t type,
                           const uint8_t* der, size_t len) {
  // AT#TLSCERTADD=<sec_id>,<type>,<data_length>,<hex_data>
  //
  // Hex-inline upload — matches every example in the TLS application note
  // (sections 4.3.3 and 4.4.3). The "data omitted, then binary after OK"
  // variant described in the AT manual is NOT honored by the modem
  // firmware in practice — sending raw bytes after the prompt produces a
  // bare "+CME ERROR" reply, observed against an ST87M0 1NCE board on
  // 2026-05-04.
  //
  // Practical cert-size limit: the modem's AT input buffer can fit
  // commands of ~2 KB. A typical ECDSA P-256 root (~250 bytes DER → 500
  // hex chars) or an RSA-2048 intermediate (~1 KB DER → 2 KB hex) fits;
  // an RSA-4096 root (e.g. ISRG Root X1 at 1391 bytes / 2782 hex) does
  // NOT — the command silently fails to complete.
  //
  // Curve restriction: the CA-cert parser only accepts ECDSA P-256 (or
  // RSA). P-384 keys (e.g. ISRG Root X2) are rejected with "+CME ERROR".
  CfunGuard rf(_modem);
  if (!rf.ok) return false;

  char prefix[48];
  snprintf(prefix, sizeof(prefix), "AT#TLSCERTADD=%u,%u,%u,",
           profileId, static_cast<unsigned>(type),
           static_cast<unsigned>(len));
  _modem.at().beginCommand(prefix);
  _modem.at().writeHex(der, len);
  _modem.at().endCommand();

  // Crypto path can take longer than a normal AT command — give it 10s.
  return _modem.at().expectOK(10000);
}

bool ST87M01TLS::uploadKey(uint8_t profileId, const uint8_t* key, size_t len) {
  // AT#TLSKEYADD=<sec_id>,<type>,<storage>,<data_flag>,<data_length>,<data>
  //
  // type = 128: asymmetric + private + SECP_R1 + hex-string output (bit7).
  // storage = 2: Flash.
  // data_flag bitmap (AT manual §14.5):
  //   bit0: 0=key content follows, 1=key generated internally
  //   bit1: 0=no public key in response, 1=return derived public key
  //   bit2: 0=DER format, 1=raw binary scalar
  //
  // We use data_flag=4: bit0=0 (import), bit1=0 (no echo), bit2=1 (raw).
  // The TLS app note §4.3 uses data_flag=2 with DER-wrapped keys; we send
  // the raw 32-byte scalar instead. The modem derives the public key
  // internally regardless of bit1 — that bit only controls the AT response.
  CfunGuard rf(_modem);
  if (!rf.ok) return false;

  char prefix[64];
  snprintf(prefix, sizeof(prefix), "AT#TLSKEYADD=%u,128,2,4,%u,",
           profileId, static_cast<unsigned>(len));
  _modem.at().beginCommand(prefix);
  _modem.at().writeHex(key, len);
  _modem.at().endCommand();

  return _modem.at().expectOK(10000);
}

// ---------------------------------------------------------------------------
// Key management
// ---------------------------------------------------------------------------
bool ST87M01TLS::deleteKey(uint8_t profileId) {
  if (profileId > MAX_PROFILE_ID) return false;
  CfunGuard rf(_modem);
  if (!rf.ok) return false;
  _modem.at().sendf("AT#TLSKEYDEL=%u", profileId);
  return _modem.at().expectOK(5000);
}

bool ST87M01TLS::listKeys(uint8_t profileId, KeyInfo* out, size_t maxItems,
                          size_t& outCount) {
  outCount = 0;
  if (!out && maxItems) return false;

  if (profileId == 0) {
    _modem.at().sendf("AT#TLSKEYLIST");
  } else {
    _modem.at().sendf("AT#TLSKEYLIST=%u", profileId);
  }

  // Response: #TLSKEYLIST: <sec_id>,<type>,<cipher>,<algo>,<size>
  // type: 0=private key, 1=PSK
  // cipher: 0=ECC (when type=0)
  // algo: 0=SECP_R1, 1=BrainpoolP256
  // size: bits
  String line;
  unsigned long deadline = millis() + 5000;
  while (millis() < deadline) {
    if (!_modem.at().readLine(line, 5000)) return false;
    if (!line.length()) continue;
    if (line == "OK") return true;
    if (line.startsWith("ERROR") || line.startsWith("+CME ERROR")) return false;
    if (!line.startsWith("#TLSKEYLIST:")) continue;

    int colon = line.indexOf(':');
    if (colon < 0) continue;
    int pos = colon + 1;
    while (pos < (int)line.length() && line.charAt(pos) == ' ') ++pos;

    // Parse comma-separated integer fields
    int comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    uint8_t fSec = static_cast<uint8_t>(line.substring(pos, comma).toInt());
    pos = comma + 1;

    comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    uint8_t fType = static_cast<uint8_t>(line.substring(pos, comma).toInt());
    pos = comma + 1;

    comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    // cipher field — skip for now
    pos = comma + 1;

    comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    uint8_t fAlgo = static_cast<uint8_t>(line.substring(pos, comma).toInt());
    pos = comma + 1;

    uint16_t fSize = static_cast<uint16_t>(line.substring(pos).toInt());

    if (outCount < maxItems) {
      KeyInfo& ki = out[outCount];
      ki.profileId = fSec;
      ki.keyType   = fType;
      ki.curve     = fAlgo;
      ki.bits      = fSize;
    }
    ++outCount;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------
bool ST87M01TLS::deleteCert(uint8_t profileId, uint8_t type) {
  if (profileId > MAX_PROFILE_ID) return false;
  CfunGuard rf(_modem);
  if (!rf.ok) return false;
  _modem.at().sendf("AT#TLSCERTDEL=%u,%u", profileId, static_cast<unsigned>(type));
  return _modem.at().expectOK(5000);
}

bool ST87M01TLS::deleteProfile(uint8_t profileId) {
  if (profileId > MAX_PROFILE_ID) return false;
  CfunGuard rf(_modem);
  if (!rf.ok) return false;
  _modem.at().sendf("AT#TLSCERTDEL=%u", profileId);
  return _modem.at().expectOK(5000);
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------
bool ST87M01TLS::listCerts(uint8_t profileId, CertInfo* out, size_t maxItems,
                           size_t& outCount) {
  outCount = 0;
  if (!out && maxItems) return false;
  if (profileId > MAX_PROFILE_ID && profileId != 0xFF && profileId != 0) return false;

  if (profileId == 0) {
    _modem.at().sendf("AT#TLSCERTLIST");
  } else {
    _modem.at().sendf("AT#TLSCERTLIST=%u", profileId);
  }

  // Firmware response (unquoted, subject before issuer — differs from manual):
  //   #TLSCERTLIST: <sec_id>,<type>,<subject>,<issuer>,<not_before>,<not_after>
  String line;
  unsigned long deadline = millis() + 5000;
  while (millis() < deadline) {
    if (!_modem.at().readLine(line, 5000)) return false;
    if (!line.length()) continue;
    if (line == "OK") return true;
    if (line.startsWith("ERROR") || line.startsWith("+CME ERROR")) return false;
    if (!line.startsWith("#TLSCERTLIST:")) continue;

    int colon = line.indexOf(':');
    if (colon < 0) continue;
    int pos = colon + 1;
    while (pos < (int)line.length() && line.charAt(pos) == ' ') ++pos;

    // Field 1: sec_id (integer)
    int comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    uint8_t fSec  = static_cast<uint8_t>(line.substring(pos, comma).toInt());
    pos = comma + 1;

    // Field 2: type (integer)
    comma = line.indexOf(',', pos);
    if (comma < 0) continue;
    uint8_t fType = static_cast<uint8_t>(line.substring(pos, comma).toInt());
    pos = comma + 1;

    // Helper: read a string field starting at pos — handles both quoted
    // ("...") and unquoted (bare comma-separated) forms. The AT manual
    // documents quotes but firmware v3.1 emits unquoted fields.
    auto readField = [&](String& dst) -> bool {
      if (pos >= (int)line.length()) return false;
      if (line.charAt(pos) == '"') {
        int end = line.indexOf('"', pos + 1);
        if (end < 0) return false;
        dst = line.substring(pos + 1, end);
        pos = end + 1;
      } else {
        int comma = line.indexOf(',', pos);
        if (comma < 0) {
          dst = line.substring(pos);
          pos = line.length();
        } else {
          dst = line.substring(pos, comma);
          pos = comma;
        }
      }
      if (pos < (int)line.length() && line.charAt(pos) == ',') ++pos;
      return true;
    };

    String issuer, subject, notBefore, notAfter;
    if (!readField(subject))   continue;
    if (!readField(issuer))    continue;
    if (!readField(notBefore)) continue;
    if (!readField(notAfter))  continue;

    if (outCount < maxItems) {
      CertInfo& ci = out[outCount];
      ci.profileId = fSec;
      ci.type      = fType;
      ci.issuer    = issuer;
      ci.subject   = subject;
      ci.notBefore = notBefore;
      ci.notAfter  = notAfter;
    }
    ++outCount;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Persist to NVM
// ---------------------------------------------------------------------------
bool ST87M01TLS::saveToNvm(unsigned long bootMs) {
  return _modem.softReset(bootMs);
}

// ---------------------------------------------------------------------------
// PEM → DER
// ---------------------------------------------------------------------------
namespace {

// Returns the base64 alphabet value (0..63) for c, -1 for whitespace/skip,
// 64 for '=' (padding terminator), -2 for invalid.
int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return 64;
  if (c == '\r' || c == '\n' || c == '\t' || c == ' ') return -1;
  return -2;
}

}  // namespace

size_t ST87M01TLS::pemToDer(const char* pem, size_t pemLen,
                            uint8_t* out, size_t outMax) {
  if (!pem || !pemLen || !out) return 0;

  // Skip past an optional "-----BEGIN ...-----" header. PEM files can carry
  // multiple blocks (cert chains) — we decode through them all and treat
  // the result as a single DER blob, which is what the modem expects when
  // chained intermediate certs are bundled.
  size_t i = 0;
  size_t outLen = 0;
  uint32_t accum = 0;
  int accumBits = 0;
  bool sawPad = false;

  while (i < pemLen) {
    // Collapse "-----...-----" markers (BEGIN/END lines) by skipping the
    // entire dashed run. PEM allows only ASCII labels inside the markers.
    if (pem[i] == '-') {
      while (i < pemLen && pem[i] != '\n') ++i;
      // Reset accumulator at every marker — a fresh BEGIN block must start
      // base64 from a clean state.
      accumBits = 0;
      accum = 0;
      sawPad = false;
      continue;
    }

    int v = b64Value(pem[i++]);
    if (v == -1) continue;            // whitespace
    if (v == -2) return 0;            // garbage in the body
    if (v == 64) {                     // '=' padding — done after this group
      sawPad = true;
      continue;
    }
    if (sawPad) return 0;              // data after padding is malformed

    accum = (accum << 6) | static_cast<uint32_t>(v);
    accumBits += 6;
    if (accumBits >= 8) {
      accumBits -= 8;
      uint8_t byte = static_cast<uint8_t>((accum >> accumBits) & 0xFF);
      if (outLen >= outMax) return 0;
      out[outLen++] = byte;
    }
  }

  return outLen;
}

// ---------------------------------------------------------------------------
// DER private key → raw 32-byte ECC scalar
// ---------------------------------------------------------------------------
// Handles two PEM/DER encodings:
//
//   SEC1 (-----BEGIN EC PRIVATE KEY-----):
//     SEQUENCE {
//       INTEGER 1                    -- version
//       OCTET STRING (32 bytes)      -- private key scalar  ← we want this
//       [0] EXPLICIT OID ...         -- named curve (optional)
//       [1] EXPLICIT BIT STRING ...  -- public key (optional)
//     }
//
//   PKCS#8 (-----BEGIN PRIVATE KEY-----):
//     SEQUENCE {
//       INTEGER 0                    -- version
//       SEQUENCE { OID ... }         -- algorithm identifier
//       OCTET STRING {               -- wraps SEC1 payload
//         SEQUENCE {
//           INTEGER 1
//           OCTET STRING (32 bytes)  ← we want this
//           ...
//         }
//       }
//     }
//
// We do a minimal walk: find the first OCTET STRING (tag 0x04) whose
// length is exactly 32. In practice, the first such octet in both SEC1
// and PKCS#8 structures IS the private key scalar.
size_t ST87M01TLS::extractEcKeyFromDer(const uint8_t* der, size_t derLen,
                                       uint8_t* out, size_t outMax) {
  if (!der || !out || outMax < 32) return 0;

  for (size_t i = 0; i + 33 <= derLen; ++i) {
    if (der[i] == 0x04 && der[i + 1] == 0x20) {
      memcpy(out, &der[i + 2], 32);
      return 32;
    }
  }
  return 0;
}
