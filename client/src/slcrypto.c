#include "slcrypto.h"

#include <string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "util.h"

static const EVP_CIPHER *cipher_for(size_t keylen, bool cbc) {
    switch (keylen) {
        case 16: return cbc ? EVP_aes_128_cbc() : EVP_aes_128_ecb();
        case 24: return cbc ? EVP_aes_192_cbc() : EVP_aes_192_ecb();
        case 32: return cbc ? EVP_aes_256_cbc() : EVP_aes_256_ecb();
        default: return NULL;
    }
}

/* Returns output length or -1. */
static int aes_run(const uint8_t *key, size_t keylen, bool cbc, const uint8_t *iv, bool encrypt, bool padding,
                   const uint8_t *in, size_t len, uint8_t *out) {
    const EVP_CIPHER *c = cipher_for(keylen, cbc);
    if (!c) return -1;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0, ok = EVP_CipherInit_ex(ctx, c, NULL, key, iv, encrypt ? 1 : 0);
    ok = ok && EVP_CIPHER_CTX_set_padding(ctx, padding ? 1 : 0);
    ok = ok && EVP_CipherUpdate(ctx, out, &n1, in, (int) len);
    ok = ok && EVP_CipherFinal_ex(ctx, out + n1, &n2);
    EVP_CIPHER_CTX_free(ctx);
    return ok ? n1 + n2 : -1;
}

void sl_random(void *out, size_t len) { RAND_bytes(out, (int) len); }

size_t sl_symmetric_encrypt(const uint8_t *key, size_t keylen, const uint8_t *plain, size_t len, uint8_t *out) {
    uint8_t iv[16];
    sl_random(iv, sizeof(iv));
    if (aes_run(key, keylen, false, NULL, true, false, iv, 16, out) != 16) return 0;
    int n = aes_run(key, keylen, true, iv, true, true, plain, len, out + 16);
    return n < 0 ? 0 : 16 + (size_t) n;
}

int sl_symmetric_decrypt(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t *out) {
    uint8_t iv[16];
    if (len < 32 || aes_run(key, keylen, false, NULL, false, false, data, 16, iv) != 16) return -1;
    return aes_run(key, keylen, true, iv, false, true, data + 16, len - 16, out);
}

size_t sl_encrypt_control(const uint8_t *key, size_t keylen, uint64_t seq, const uint8_t *payload, size_t len,
                          uint8_t *out) {
    uint8_t plain[2048];
    if (len + 8 > sizeof(plain)) return 0;
    put_le64(plain, seq);
    memcpy(plain + 8, payload, len);
    unsigned int maclen = 16;
    HMAC(EVP_md5(), key, (int) keylen, plain, len + 8, out, &maclen);
    int n = aes_run(key, keylen, true, out, true, true, plain, len + 8, out + 16);
    return n < 0 ? 0 : 16 + (size_t) n;
}

int sl_decrypt_control(const uint8_t *key, size_t keylen, const uint8_t *body, size_t len, uint64_t *seq,
                       uint8_t *out) {
    static __thread uint8_t plain[65536];
    if (len < 32 || len - 16 > sizeof(plain)) return -1;
    int n = aes_run(key, keylen, true, body, false, true, body + 16, len - 16, plain);
    if (n < 8) return -1;
    uint8_t mac[16];
    unsigned int maclen = 16;
    HMAC(EVP_md5(), key, (int) keylen, plain, (size_t) n, mac, &maclen);
    if (CRYPTO_memcmp(mac, body, 16) != 0) return -1;
    *seq = (uint64_t) get_le32(plain) | (uint64_t) get_le32(plain + 4) << 32;
    memcpy(out, plain + 8, (size_t) n - 8);
    return n - 8;
}

int sl_decrypt_zero_iv(const uint8_t *key, size_t keylen, const uint8_t *data, size_t len, uint8_t *out) {
    static const uint8_t zero[16];
    return aes_run(key, keylen, true, zero, false, true, data, len, out);
}

void sl_sha256(const void *data, size_t len, uint8_t out[32]) {
    unsigned int n = 32;
    EVP_Digest(data, len, out, &n, EVP_sha256(), NULL);
}

bool sl_x25519_keypair(uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    EVP_PKEY *key = NULL;
    size_t pl = 32, ql = 32;
    bool ok = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &key) > 0 &&
              EVP_PKEY_get_raw_private_key(key, priv, &pl) > 0 && EVP_PKEY_get_raw_public_key(key, pub, &ql) > 0;
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
    return ok && pl == 32 && ql == 32;
}

bool sl_x25519_shared_sha256(const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    EVP_PKEY *mine = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    EVP_PKEY *theirs = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer, 32);
    EVP_PKEY_CTX *ctx = mine ? EVP_PKEY_CTX_new(mine, NULL) : NULL;
    uint8_t secret[32];
    size_t sl = sizeof(secret);
    bool ok = ctx && theirs && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_derive_set_peer(ctx, theirs) > 0 &&
              EVP_PKEY_derive(ctx, secret, &sl) > 0 && sl == 32;
    if (ok) sl_sha256(secret, 32, out);
    OPENSSL_cleanse(secret, sizeof(secret));
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(mine);
    EVP_PKEY_free(theirs);
    return ok;
}

int sl_rsa_oaep_encrypt(const char *pem, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    BIO *bio = BIO_new_mem_buf(pem, -1);
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    BIO_free(bio);
    if (!key) return -1;
    size_t block = (size_t) EVP_PKEY_size(key) - 42, total = 0;
    int ret = 0;
    for (size_t off = 0; off < len && ret == 0; off += block) {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(key, NULL);
        size_t chunk = len - off < block ? len - off : block, outl = cap - total;
        if (!ctx || EVP_PKEY_encrypt_init(ctx) <= 0 || EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0 ||
            EVP_PKEY_encrypt(ctx, out + total, &outl, in + off, chunk) <= 0)
            ret = -1;
        else
            total += outl;
        EVP_PKEY_CTX_free(ctx);
    }
    EVP_PKEY_free(key);
    return ret ? -1 : (int) total;
}

void sl_auth_token(const uint8_t *session_key, size_t keylen, uint8_t out[32]) {
    static const char magic[] = "Steam In-Home Streaming";
    unsigned int n = 32;
    HMAC(EVP_sha256(), session_key, (int) keylen, (const uint8_t *) magic, sizeof(magic) - 1, out, &n);
}
