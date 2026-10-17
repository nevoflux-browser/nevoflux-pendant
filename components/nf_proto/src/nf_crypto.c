#include "nf_proto.h"
#include <string.h>

#if defined(ESP_PLATFORM)
#include "mbedtls/gcm.h"
#else
#include <openssl/evp.h>
#endif

size_t nf_channel_encrypt(const uint8_t key[NF_AES_KEY_SIZE],
                          const uint8_t nonce[NF_AES_NONCE_SIZE],
                          const uint8_t *plaintext,
                          size_t pt_len,
                          uint8_t *out_wire,
                          size_t out_cap)
{
    if (!key || !nonce || !out_wire) {
        return 0U;
    }
    if (pt_len > 0U && !plaintext) {
        return 0U;
    }
    size_t total_len = pt_len + NF_AES_OVERHEAD;
    if (out_cap < total_len) {
        return 0U;
    }

    memcpy(out_wire, nonce, NF_AES_NONCE_SIZE);
    uint8_t *ct_dst = out_wire + NF_AES_NONCE_SIZE;
    uint8_t *tag_dst = out_wire + NF_AES_NONCE_SIZE + pt_len;

#if defined(ESP_PLATFORM)
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&ctx,
                                       MBEDTLS_GCM_ENCRYPT,
                                       pt_len,
                                       nonce,
                                       NF_AES_NONCE_SIZE,
                                       NULL,
                                       0,
                                       plaintext,
                                       ct_dst,
                                       NF_AES_TAG_SIZE,
                                       tag_dst);
    }
    mbedtls_gcm_free(&ctx);
    return (rc == 0) ? total_len : 0U;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return 0U;
    }
    int len = 0;
    bool ok = (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1) &&
              (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)NF_AES_NONCE_SIZE, NULL) == 1) &&
              (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1);
    if (ok && pt_len > 0U) {
        ok = (EVP_EncryptUpdate(ctx, ct_dst, &len, plaintext, (int)pt_len) == 1);
    }
    int final_len = 0;
    if (ok) {
        ok = (EVP_EncryptFinal_ex(ctx, ct_dst + len, &final_len) == 1) &&
             (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, (int)NF_AES_TAG_SIZE, tag_dst) == 1);
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok ? total_len : 0U;
#endif
}

bool nf_channel_decrypt(const uint8_t key[NF_AES_KEY_SIZE],
                        const uint8_t *wire,
                        size_t wire_len,
                        uint8_t *out_plaintext,
                        size_t out_cap,
                        size_t *out_pt_len)
{
    if (!key || !wire || !out_pt_len || wire_len < NF_AES_OVERHEAD) {
        return false;
    }
    size_t pt_len = wire_len - NF_AES_OVERHEAD;
    if (pt_len > 0U && (!out_plaintext || out_cap < pt_len)) {
        return false;
    }

    const uint8_t *nonce = wire;
    const uint8_t *ct = wire + NF_AES_NONCE_SIZE;
    const uint8_t *tag = wire + NF_AES_NONCE_SIZE + pt_len;

#if defined(ESP_PLATFORM)
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&ctx,
                                      pt_len,
                                      nonce,
                                      NF_AES_NONCE_SIZE,
                                      NULL,
                                      0,
                                      tag,
                                      NF_AES_TAG_SIZE,
                                      ct,
                                      out_plaintext);
    }
    mbedtls_gcm_free(&ctx);
    if (rc != 0) {
        return false;
    }
    *out_pt_len = pt_len;
    return true;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return false;
    }
    int len = 0;
    bool ok = (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1) &&
              (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)NF_AES_NONCE_SIZE, NULL) == 1) &&
              (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1);
    if (ok && pt_len > 0U) {
        ok = (EVP_DecryptUpdate(ctx, out_plaintext, &len, ct, (int)pt_len) == 1);
    }
    if (ok) {
        uint8_t tag_copy[NF_AES_TAG_SIZE];
        memcpy(tag_copy, tag, NF_AES_TAG_SIZE);
        ok = (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)NF_AES_TAG_SIZE, tag_copy) == 1);
    }
    int final_len = 0;
    if (ok) {
        ok = (EVP_DecryptFinal_ex(ctx, out_plaintext ? (out_plaintext + len) : NULL, &final_len) == 1);
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return false;
    }
    *out_pt_len = pt_len;
    return true;
#endif
}
