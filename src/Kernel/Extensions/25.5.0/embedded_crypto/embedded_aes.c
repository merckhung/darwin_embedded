/* Adapt BearSSL's portable constant-time AES to XNU's CBC callback interface. */
#include <sys/systm.h>
#include <corecrypto/cc_priv.h>
#include <libkern/crypto/aes.h>
#include "bearssl_block.h"
#include "embedded_aes.h"
#include "aes_selftest_vectors.h"

_Static_assert(sizeof(br_aes_ct_cbcenc_keys) <= sizeof(aes_encrypt_ctx), "AES encrypt context bounds");
_Static_assert(sizeof(br_aes_ct_cbcdec_keys) <= sizeof(aes_decrypt_ctx), "AES decrypt context bounds");
_Static_assert(sizeof(aes_kat_plaintext) == 64, "CBC known-answer input length");

static int encrypt_init(const struct ccmode_cbc *mode, cccbc_ctx *context,
                        size_t key_size, const void *key) {
    (void)mode;
    cc_clear(sizeof(br_aes_ct_cbcenc_keys), context);
    if (!key || (key_size != 16 && key_size != 24 && key_size != 32)) return -1;
    br_aes_ct_cbcenc_init((br_aes_ct_cbcenc_keys *)context, key, key_size);
    return 0;
}
static int decrypt_init(const struct ccmode_cbc *mode, cccbc_ctx *context,
                        size_t key_size, const void *key) {
    (void)mode;
    cc_clear(sizeof(br_aes_ct_cbcdec_keys), context);
    if (!key || (key_size != 16 && key_size != 24 && key_size != 32)) return -1;
    br_aes_ct_cbcdec_init((br_aes_ct_cbcdec_keys *)context, key, key_size);
    return 0;
}
static int encrypt_blocks(const cccbc_ctx *context, cccbc_iv *iv,
                          size_t blocks, const void *input, void *output) {
    const br_aes_ct_cbcenc_keys *keys = (const br_aes_ct_cbcenc_keys *)context;
    if (!iv || blocks > SIZE_MAX / 16 || (blocks && (!input || !output)) ||
        (keys->num_rounds != 10 && keys->num_rounds != 12 && keys->num_rounds != 14)) return -1;
    if (!blocks) return 0;
    memmove(output, input, blocks * 16);
    br_aes_ct_cbcenc_run(keys, iv, output, blocks * 16);
    return 0;
}
static int decrypt_blocks(const cccbc_ctx *context, cccbc_iv *iv,
                          size_t blocks, const void *input, void *output) {
    const br_aes_ct_cbcdec_keys *keys = (const br_aes_ct_cbcdec_keys *)context;
    if (!iv || blocks > SIZE_MAX / 16 || (blocks && (!input || !output)) ||
        (keys->num_rounds != 10 && keys->num_rounds != 12 && keys->num_rounds != 14)) return -1;
    if (!blocks) return 0;
    memmove(output, input, blocks * 16);
    br_aes_ct_cbcdec_run(keys, iv, output, blocks * 16);
    return 0;
}
const struct ccmode_cbc embedded_aes_cbc_encrypt = {
    .size = sizeof(br_aes_ct_cbcenc_keys), .block_size = 16,
    .init = encrypt_init, .cbc = encrypt_blocks,
};
const struct ccmode_cbc embedded_aes_cbc_decrypt = {
    .size = sizeof(br_aes_ct_cbcdec_keys), .block_size = 16,
    .init = decrypt_init, .cbc = decrypt_blocks,
};

void embedded_aes_selftest(void) {
    cccbc_ctx_decl(sizeof(br_aes_ct_cbcenc_keys), enc);
    cccbc_ctx_decl(sizeof(br_aes_ct_cbcdec_keys), dec);
    cccbc_iv_decl(16, iv);
    uint8_t output[64];
    for (unsigned index = 0; index < 3; ++index) {
        const size_t key_size = 16 + index * 8;
        memcpy(iv, aes_kat_iv, 16);
        if (encrypt_init(&embedded_aes_cbc_encrypt, enc, key_size, aes_kat_keys[index]) ||
            encrypt_blocks(enc, iv, 4, aes_kat_plaintext, output) ||
            cc_cmp_safe(sizeof(output), output, aes_kat_ciphertext[index]) ||
            cc_cmp_safe(16, iv, aes_kat_ciphertext[index] + 48))
            panic("embedded crypto: AES-%zu CBC encryption self-test failed", key_size * 8);
        memcpy(iv, aes_kat_iv, 16);
        if (decrypt_init(&embedded_aes_cbc_decrypt, dec, key_size, aes_kat_keys[index]) ||
            decrypt_blocks(dec, iv, 4, output, output) ||
            cc_cmp_safe(sizeof(output), output, aes_kat_plaintext) ||
            cc_cmp_safe(16, iv, aes_kat_ciphertext[index] + 48))
            panic("embedded crypto: AES-%zu CBC decryption self-test failed", key_size * 8);
        /* Check chaining across calls, including the CBC decrypt odd-block path. */
        memcpy(iv, aes_kat_iv, 16);
        if (decrypt_blocks(dec, iv, 1, aes_kat_ciphertext[index], output) ||
            decrypt_blocks(dec, iv, 3, aes_kat_ciphertext[index] + 16, output + 16) ||
            cc_cmp_safe(sizeof(output), output, aes_kat_plaintext))
            panic("embedded crypto: AES CBC chaining self-test failed");
    }
    if (!encrypt_init(&embedded_aes_cbc_encrypt, enc, 15, aes_kat_keys[0]) ||
        !encrypt_blocks(enc, iv, 1, aes_kat_plaintext, output))
        panic("embedded crypto: invalid AES key accepted");
    cc_clear(sizeof(br_aes_ct_cbcenc_keys), enc);
    cc_clear(sizeof(br_aes_ct_cbcdec_keys), dec);
    cc_clear(16, iv);
    cc_clear(sizeof(output), output);
    printf("Embedded AES-128/192/256 CBC known-answer tests passed\n");
}
