// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 MtlsGet — mutual TLS (client-certificate authentication)

  Provisions a CA root certificate, an ECC P-256 client private key,
  and a client certificate into modem security profile 1, then opens
  an HTTPS session and performs a GET request.

  This demonstrates "Path A" mTLS: importing an externally generated
  private key + client certificate. The modem validates that the client
  cert matches the private key at import time — import the key first.

  Provisioning order matters:
    1. CA root cert    (tls.addCaCertPem)
    2. Private key     (tls.addPrivateKey or tls.addPrivateKeyPem)
    3. Client cert     (tls.addClientCertPem)
    4. Optionally persist to NVM (tls.saveToNvm)

  All Phase 1 constraints still apply — see HttpsGet.ino header for
  the full list (chain validation, ECDSA-only parser, CFUN=4, etc.).
  Additionally for mTLS:
    * The private key must be ECC SECP R1 (P-256), raw 32-byte scalar.
      The addPrivateKeyPem() overload extracts the scalar from PEM-
      encoded PKCS#8 or SEC1 (EC PRIVATE KEY) blocks automatically.
    * The modem validates key-cert correspondence at client cert import
      time — a mismatch returns CME ERROR.
    * ITS storage limit: max 10 files total across all profiles. A full
      mTLS profile (CA + key + client cert + header) uses 4 files,
      leaving room for one more profile.

  TO USE THIS EXAMPLE: replace the placeholder constants below with
  your own server, CA cert, client cert, and private key. The
  placeholders will compile but fail at provisioning time.

  Generating test credentials (OpenSSL):
    # CA key + self-signed root
    openssl ecparam -name prime256v1 -genkey -noout -out ca-key.pem
    openssl req -new -x509 -key ca-key.pem -sha256 -days 3650 \
        -out ca-cert.pem -subj "/CN=Test CA"
    # Client key + CSR + cert signed by the CA
    openssl ecparam -name prime256v1 -genkey -noout -out client-key.pem
    openssl req -new -key client-key.pem -sha256 \
        -out client.csr -subj "/CN=ST87M01 Device"
    openssl x509 -req -in client.csr -CA ca-cert.pem -CAkey ca-key.pem \
        -CAcreateserial -sha256 -days 3650 -out client-cert.pem
    # Extract raw 32-byte private key scalar (hex):
    openssl ec -in client-key.pem -text -noout 2>/dev/null | \
        grep -A3 'priv:' | tail -3 | tr -d ' :\n'

  Hardware: any board with a ST87M01Boards.h preset. Run ConfigureDns
  once per device so AT#DNS has a server to query.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01HTTP.h>
#include <ST87M01TLS.h>
#include <ST87M01Boards.h>

// ---- User configuration ------------------------------------------------
static const char* APN  = "iot.1nce.net";
static const char* HOST = "your-server.example.com";
static const char* PATH_ = "/";
static constexpr uint16_t PORT = 8443;

static constexpr uint8_t SEC_PROFILE = 1;
static constexpr bool SAVE_TO_NVM = false;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long RESPONSE_TIMEOUT_MS     = 30000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 10000;
static constexpr bool AT_DEBUG = false;

// ---- Certificates and key (REPLACE these) ------------------------------

// CA certificate that signed your server's leaf cert (must be ECDSA P-256).
static const char CA_CERT_PEM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "REPLACE_WITH_YOUR_CA_CERT_BASE64\n"
  "-----END CERTIFICATE-----\n";

// Client private key — PEM-encoded SEC1 (EC PRIVATE KEY).
// addPrivateKeyPem() extracts the raw 32-byte scalar automatically.
static const char CLIENT_KEY_PEM[] =
  "-----BEGIN EC PRIVATE KEY-----\n"
  "REPLACE_WITH_YOUR_CLIENT_KEY_BASE64\n"
  "-----END EC PRIVATE KEY-----\n";

// Client certificate signed by a CA the server trusts (ECDSA P-256).
static const char CLIENT_CERT_PEM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "REPLACE_WITH_YOUR_CLIENT_CERT_BASE64\n"
  "-----END CERTIFICATE-----\n";

// ---- Global objects ----------------------------------------------------
ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01TLS     tls(modem);
ST87M01HTTP    https(modem);

static const char* regStatusStr(ST87M01RegStatus r) {
  switch (r) {
    case ST87M01RegStatus::NotRegistered:      return "not registered";
    case ST87M01RegStatus::RegisteredHome:     return "registered (home)";
    case ST87M01RegStatus::Searching:          return "searching";
    case ST87M01RegStatus::RegistrationDenied: return "denied";
    case ST87M01RegStatus::RegisteredRoaming:  return "registered (roaming)";
    default:                                   return "unknown";
  }
}

static bool isRegistered(ST87M01RegStatus r) {
  return r == ST87M01RegStatus::RegisteredHome ||
         r == ST87M01RegStatus::RegisteredRoaming;
}

static bool waitForRegistration(unsigned long timeoutMs) {
  unsigned long start = millis();
  unsigned long lastLog = 0;

  while ((millis() - start) < timeoutMs) {
    modem.poll();

    ST87M01CellInfo cell;
    bool ok = modem.getCellInfo(cell);
    if (ok && isRegistered(cell.reg)) return true;

    if (millis() - lastLog >= 5000) {
      lastLog = millis();
      Serial.print(F("  ["));
      Serial.print((millis() - start) / 1000);
      Serial.print(F("s] "));
      Serial.println(ok ? regStatusStr(cell.reg) : "(no response)");
    }
    delay(500);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 MtlsGet (mutual TLS) ==="));

  if (AT_DEBUG) modem.at().setDebugStream(&Serial);

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }
  modem.setCeregMode(2);

  // -- Provision credentials before network attach --
  Serial.println(F("Provisioning mTLS credentials into profile 1..."));

  // Step 1: CA root cert
  Serial.print(F("  CA cert... "));
  if (!tls.addCaCertPem(SEC_PROFILE, CA_CERT_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.println(F("OK"));

  // Step 2: Client private key (must come before client cert)
  Serial.print(F("  Private key... "));
  if (!tls.addPrivateKeyPem(SEC_PROFILE, CLIENT_KEY_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.println(F("OK"));

  // Step 3: Client certificate (modem validates key-cert match here)
  Serial.print(F("  Client cert... "));
  if (!tls.addClientCertPem(SEC_PROFILE, CLIENT_CERT_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    Serial.println(F("  (key-cert mismatch? wrong curve? cert too large?)"));
    return;
  }
  Serial.println(F("OK"));

  if (SAVE_TO_NVM) {
    Serial.println(F("Saving to NVM (modem will reboot)..."));
    if (!tls.saveToNvm()) {
      Serial.println(F("saveToNvm failed."));
      return;
    }
    modem.setCeregMode(2);
  }

  // -- Network attach --
  Serial.println(F("Waiting for registration..."));
  if (!waitForRegistration(REGISTRATION_TIMEOUT_MS)) {
    Serial.println(F("Not registered."));
    return;
  }

  if (!network.begin(APN)) {
    Serial.print(F("network.begin() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  Serial.print(F("Network up. cid="));
  Serial.print(network.cid());
  Serial.print(F("  IP="));
  Serial.println(network.localIP());

  // -- HTTPS request --
  Serial.print(F("Opening mTLS session to "));
  Serial.print(HOST);
  Serial.print(':');
  Serial.println(PORT);

  if (!https.begin(HOST, PORT, SEC_PROFILE)) {
    Serial.print(F("https.begin() failed — CME "));
    Serial.print(modem.at().lastCmeError());
    Serial.print(F("  TLS err="));
    Serial.println(https.lastTlsError());
    return;
  }

  https.addHeader("User-Agent", "ST87M01/1.0");

  Serial.print(F("GET "));
  Serial.println(PATH_);

  if (!https.get(PATH_, RESPONSE_TIMEOUT_MS)) {
    Serial.print(F("https.get() failed — CME "));
    Serial.print(modem.at().lastCmeError());
    Serial.print(F("  TLS err="));
    Serial.println(https.lastTlsError());
    https.end();
    return;
  }

  Serial.print(F("Status: "));
  Serial.println(https.statusCode());
  Serial.print(F("Content-Length: "));
  Serial.println(https.contentLength());

  Serial.println(F("--- headers ---"));
  Serial.print(https.rawHeaders());
  Serial.println(F("--- body ---"));

  unsigned long lastByte = millis();
  size_t rxCount = 0;

  while (!https.eof()) {
    modem.poll();

    int n = https.available();
    if (n > 0) {
      while (https.available() > 0) {
        int b = https.read();
        if (b < 0) break;
        Serial.write(static_cast<uint8_t>(b));
        rxCount++;
        lastByte = millis();
      }
    } else {
      if ((millis() - lastByte) >= READ_IDLE_TIMEOUT_MS) {
        Serial.println(F("\n[idle timeout waiting for more body]"));
        break;
      }
      delay(10);
    }
  }

  Serial.println(F("--- end ---"));
  Serial.print(F("Received "));
  Serial.print(rxCount);
  Serial.println(F(" body bytes."));

  https.end();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
