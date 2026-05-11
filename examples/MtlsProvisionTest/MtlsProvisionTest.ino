// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  MtlsProvisionTest — validate mTLS credential provisioning on hardware.

  No server needed. This sketch:
    1. Provisions a CA cert, ECC P-256 private key, and client cert
    2. Lists certs and keys back via AT#TLSCERTLIST / AT#TLSKEYLIST
    3. Cleans up by deleting the profile

  Tests the AT#TLSKEYADD (data_flag=4, raw binary) and AT#TLSCERTADD
  (type=0, client cert with key-cert validation) code paths.
*/

#include <ST87M01Modem.h>
#include <ST87M01TLS.h>
#include <ST87M01Boards.h>

static constexpr uint8_t SEC_PROFILE = 1;

ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01TLS   tls(modem);

// Test CA — self-signed ECC P-256
static const char CA_CERT_PEM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIBeDCCAR+gAwIBAgIUIzUG/H2ABhJjZI/dwPsetCbn6vUwCgYIKoZIzj0EAwIw\n"
  "EjEQMA4GA1UEAwwHVGVzdCBDQTAeFw0yNjA1MDUwNzU1MzdaFw0zNjA1MDIwNzU1\n"
  "MzdaMBIxEDAOBgNVBAMMB1Rlc3QgQ0EwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNC\n"
  "AAQMBSk+QPqQS90CJhy5Zop5/yRqUkmCZSENjquMRpgO/hbuWLRB90YgkMtAX/WK\n"
  "q5HhrkzqUutn5RiZ343b1rkUo1MwUTAdBgNVHQ4EFgQUAiBG8STvIJ7iR5hTfESI\n"
  "hhX5e6MwHwYDVR0jBBgwFoAUAiBG8STvIJ7iR5hTfESIhhX5e6MwDwYDVR0TAQH/\n"
  "BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiAT+k7WGpy64FpiLHW5KzqZv4rUcFul\n"
  "HZf2j3Ac5CGMhAIgNrh9pzSoNKfHmh8Ab1h4uMz0VlJAnFubpaQKR6ysAPM=\n"
  "-----END CERTIFICATE-----\n";

// Client private key — ECC P-256 (SEC1 / EC PRIVATE KEY format)
static const char CLIENT_KEY_PEM[] =
  "-----BEGIN EC PRIVATE KEY-----\n"
  "MHcCAQEEIMyp+lFyjQmR6VpzHzgQ2Sx4/HWr/zIOxY0hxblbEHt2oAoGCCqGSM49\n"
  "AwEHoUQDQgAEHBCaE76kpniNDrK8reebA+8Lf87BBhTQSmXBaAyXsTznGG5m+yN/\n"
  "zmLvF7yaizF1/4NeJ3BvsSkrS34LpIKtVw==\n"
  "-----END EC PRIVATE KEY-----\n";

// Client certificate — signed by the test CA above
static const char CLIENT_CERT_PEM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIBJTCBzAIUacKkxqaZvmOyz5myBXWyeBlpTG4wCgYIKoZIzj0EAwIwEjEQMA4G\n"
  "A1UEAwwHVGVzdCBDQTAeFw0yNjA1MDUwNzU1MzdaFw0zNjA1MDIwNzU1MzdaMBkx\n"
  "FzAVBgNVBAMMDlNUODdNMDEgRGV2aWNlMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcD\n"
  "QgAEHBCaE76kpniNDrK8reebA+8Lf87BBhTQSmXBaAyXsTznGG5m+yN/zmLvF7ya\n"
  "izF1/4NeJ3BvsSkrS34LpIKtVzAKBggqhkjOPQQDAgNIADBFAiEAqVuiBgI0WhlZ\n"
  "RW7WodTe0vVW3qCk8YrcW7ULZ2FlfbwCIGznvXZ7U/CGNTeQ2oMpGtyHkXvHt7VV\n"
  "TVog7eQ4PYt4\n"
  "-----END CERTIFICATE-----\n";

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== mTLS Provisioning Test ==="));
  Serial.println();

  modem.at().setDebugStream(&Serial);

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }

  // ---- Step 1: Provision CA cert ------------------------------------
  Serial.println(F("--- Step 1: CA cert ---"));
  if (!tls.addCaCertPem(SEC_PROFILE, CA_CERT_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.println(F("OK"));
  Serial.println();

  // ---- Step 2: Private key ------------------------------------------
  Serial.println(F("--- Step 2: Private key (PEM) ---"));
  if (!tls.addPrivateKeyPem(SEC_PROFILE, CLIENT_KEY_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    Serial.println(F("(If CME 0, the PEM decode or raw-key extract failed host-side)"));
    return;
  }
  Serial.println(F("OK"));
  Serial.println();

  // ---- Step 3: Client cert ------------------------------------------
  Serial.println(F("--- Step 3: Client cert ---"));
  if (!tls.addClientCertPem(SEC_PROFILE, CLIENT_CERT_PEM)) {
    Serial.print(F("FAILED — CME "));
    Serial.println(modem.at().lastCmeError());
    Serial.println(F("(Key-cert mismatch? Cert too large?)"));
    return;
  }
  Serial.println(F("OK"));
  Serial.println();

  // ---- Step 4: List certs -------------------------------------------
  Serial.println(F("--- Step 4: List certs in profile ---"));
  ST87M01TLS::CertInfo certs[4];
  size_t certCount = 0;
  if (tls.listCerts(SEC_PROFILE, certs, 4, certCount)) {
    Serial.print(F("Found "));
    Serial.print(certCount);
    Serial.println(F(" cert(s):"));
    for (size_t i = 0; i < certCount && i < 4; ++i) {
      Serial.print(F("  ["));
      Serial.print(certs[i].profileId);
      Serial.print(F("] type="));
      Serial.print(certs[i].type);
      Serial.print(F("  issuer=\""));
      Serial.print(certs[i].issuer);
      Serial.print(F("\"  subject=\""));
      Serial.print(certs[i].subject);
      Serial.println(F("\""));
    }
  } else {
    Serial.println(F("listCerts failed"));
  }
  Serial.println();

  // ---- Step 5: List keys --------------------------------------------
  Serial.println(F("--- Step 5: List keys in profile ---"));
  ST87M01TLS::KeyInfo keys[2];
  size_t keyCount = 0;
  if (tls.listKeys(SEC_PROFILE, keys, 2, keyCount)) {
    Serial.print(F("Found "));
    Serial.print(keyCount);
    Serial.println(F(" key(s):"));
    for (size_t i = 0; i < keyCount && i < 2; ++i) {
      Serial.print(F("  ["));
      Serial.print(keys[i].profileId);
      Serial.print(F("] type="));
      Serial.print(keys[i].keyType);
      Serial.print(F("  curve="));
      Serial.print(keys[i].curve);
      Serial.print(F("  bits="));
      Serial.println(keys[i].bits);
    }
  } else {
    Serial.println(F("listKeys failed"));
  }
  Serial.println();

  // ---- Step 6: Cleanup ----------------------------------------------
  Serial.println(F("--- Step 6: Cleanup ---"));
  Serial.print(F("  deleteKey: "));
  Serial.println(tls.deleteKey(SEC_PROFILE) ? "OK" : "FAILED");
  Serial.print(F("  deleteProfile: "));
  Serial.println(tls.deleteProfile(SEC_PROFILE) ? "OK" : "FAILED");

  Serial.println();
  Serial.println(F("=== Test complete ==="));
}

void loop() {}
