#pragma once
/* Valve crypto primitives used by Steam Link (see docs/PROTOCOLO.md and tools/proto/slcrypto.py). */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void sl_random(void *out, size_t len);

/* CCrypto::SymmetricEncrypt: AES-ECB(key, iv) || AES-CBC(key, iv, PKCS7(plain)). out needs len + 32 bytes. */
size_t sl_symmetric_encrypt(const uint8_t *key, size_t keylen, const uint8_t *plain, size_t len, uint8_t *out);
/* Returns plaintext length or -1. out needs len bytes. */
int sl_symmetric_decrypt(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t *out);

/* Control channel: HMAC-MD5(key, seq||payload) as IV || AES-CBC(key, IV, PKCS7(seq_le64||payload)). */
size_t sl_encrypt_control(const uint8_t *key, size_t keylen, uint64_t seq, const uint8_t *payload, size_t len,
                          uint8_t *out);
/* Returns payload length (payload written to out) or -1 on HMAC/padding failure. */
int sl_decrypt_control(const uint8_t *key, size_t keylen, const uint8_t *body, size_t len, uint64_t *seq,
                       uint8_t *out);

/* AES-CBC with a zero IV and PKCS7, used for encrypted video frames. Returns length or -1. */
int sl_decrypt_zero_iv(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t *out);

void sl_auth_token(const uint8_t *session_key, size_t keylen, uint8_t out[32]);

void sl_sha256(const void *data, size_t len, uint8_t out[32]);

/* X25519 pair; the pairing secret is SHA256(X25519(priv, peer)). */
bool sl_x25519_keypair(uint8_t priv[32], uint8_t pub[32]);
bool sl_x25519_shared_sha256(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]);

/* RSA-OAEP(SHA1) in blocks of RSA_size - 42 bytes, concatenated. Returns bytes written or -1. */
int sl_rsa_oaep_encrypt(const char *pem, const uint8_t *in, size_t len, uint8_t *out, size_t cap);
