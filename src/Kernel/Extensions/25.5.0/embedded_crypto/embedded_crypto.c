/* Minimal boot crypto provider using the current kernel's real SHA-2/HMAC DRBG.
 * This is not Apple's corecrypto module or a FIPS-certified replacement.
 * AES-CBC uses the pinned BearSSL implementation. Legacy MD5/SHA-1 provide filesystem/ABI compatibility. RSA is unsupported.
 */
#include <sys/systm.h>
#include <mach/kmod.h>
#include <kern/locks.h>
#include <libkern/crypto/register_crypto.h>
#include <corecrypto/cckprng.h>
#include <corecrypto/ccdrbg.h>
#include <corecrypto/ccsha2.h>
#include <corecrypto/cc_priv.h>
#include "selftest_vectors.h"
#include "embedded_aes.h"
#include "embedded_legacy_digest.h"

extern void register_and_init_prng(struct cckprng_ctx *, const struct cckprng_funcs *);
extern const struct ccdigest_info ccsha256_ltc_di, ccsha512_ltc_di;

struct embedded_prng {
    struct ccdrbg_info info;
    struct ccdrbg_nisthmac_custom custom;
    uint8_t state[CCKPRNG_DRBG_STATE_MAX_SIZE] __attribute__((aligned(16)));
    cckprng_getentropy getentropy;
    void *entropy_arg;
    lck_grp_t *group;
    lck_spin_t *lock;
};
static struct embedded_prng generator;

static struct embedded_prng *prng(struct cckprng_ctx *context) {
    return (struct embedded_prng *)context;
}
static void check_drbg(int status) {
    if (status != CCDRBG_STATUS_OK) panic("embedded crypto: DRBG error %d", status);
}
static void prng_init(struct cckprng_ctx *context, size_t seed_size, const void *seed,
                      size_t nonce_size, const void *nonce,
                      cckprng_getentropy getentropy, void *entropy_arg) {
    struct embedded_prng *g = prng(context);
    if (seed_size < 32 || !seed) panic("embedded crypto: insufficient initial seed");
    g->custom.di = &ccsha256_ltc_di;
    g->custom.strictFIPS = 0;
    g->getentropy = getentropy;
    g->entropy_arg = entropy_arg;
    ccdrbg_factory_nisthmac(&g->info, &g->custom);
    if (g->info.size > sizeof(g->state)) panic("embedded crypto: DRBG state bounds");
    g->group = lck_grp_alloc_init("embedded crypto", LCK_GRP_ATTR_NULL);
    if (!g->group) panic("embedded crypto: cannot allocate lock group");
    g->lock = lck_spin_alloc_init(g->group, LCK_ATTR_NULL);
    if (!g->lock) panic("embedded crypto: cannot allocate lock");
    const char personalization[] = "Darwin embedded shared kernel PRNG";
    check_drbg(ccdrbg_init(&g->info, (struct ccdrbg_state *)g->state,
                          seed_size, seed, nonce_size, nonce,
                          sizeof(personalization) - 1, personalization));
}
static void prng_init_entropy(struct cckprng_ctx *context, unsigned generators,
                              size_t seed_size, const void *seed, size_t nonce_size,
                              const void *nonce, cckprng_getentropy getentropy,
                              void *entropy_arg) {
    (void)generators;
    prng_init(context, seed_size, seed, nonce_size, nonce, getentropy, entropy_arg);
}
static void prng_initgen(struct cckprng_ctx *context, unsigned cpu) {
    (void)cpu;
    if (!prng(context)->lock) panic("embedded crypto: PRNG not initialized");
}
static void prng_reseed(struct cckprng_ctx *context, size_t size, const void *seed) {
    struct embedded_prng *g = prng(context);
    if (size < 16 || !seed) panic("embedded crypto: insufficient reseed input");
    lck_spin_lock(g->lock);
    int status = ccdrbg_reseed(&g->info, (struct ccdrbg_state *)g->state,
                             size, seed, 0, NULL);
    lck_spin_unlock(g->lock);
    check_drbg(status);
}
static void prng_refresh(struct cckprng_ctx *context) {
    struct embedded_prng *g = prng(context);
    if (g->getentropy) {
        uint8_t entropy[CCKPRNG_ENTROPY_SIZE];
        size_t size = sizeof(entropy);
        int32_t samples = g->getentropy(&size, entropy, g->entropy_arg);
        if (size > sizeof(entropy)) panic("embedded crypto: entropy callback bounds");
        if (samples < 0) panic("embedded crypto: entropy health failure");
        if (samples > 0 && size >= 16) prng_reseed(context, size, entropy);
        cc_clear(sizeof(entropy), entropy);
    }
}
static void prng_generate(struct cckprng_ctx *context, unsigned cpu,
                          size_t size, void *output) {
    (void)cpu;
    struct embedded_prng *g = prng(context);
    uint8_t *bytes = output;
    while (size) {
        size_t amount = MIN(size, CCKPRNG_MAX_REQUEST_SIZE);
        lck_spin_lock(g->lock);
        int status = ccdrbg_generate(&g->info, (struct ccdrbg_state *)g->state,
                                    amount, bytes, 0, NULL);
        lck_spin_unlock(g->lock);
        check_drbg(status);
        bytes += amount;
        size -= amount;
    }
}
static const struct cckprng_funcs prng_functions = {
    .init = prng_init, .initgen = prng_initgen, .reseed = prng_reseed,
    .refresh = prng_refresh, .generate = prng_generate,
    .init_with_getentropy = prng_init_entropy,
};

static int rng_generate(struct ccrng_state *rng, size_t size, void *output) {
    (void)rng;
    prng_generate((struct cckprng_ctx *)&generator, 0, size, output);
    return 0;
}
static struct ccrng_state rng_state = {.generate = rng_generate};
static struct ccrng_state *rng_get(int *error) {
    if (error) *error = 0;
    return &rng_state;
}
/* The kmem contexts use the same locked generator rather than unseeded pools. */
static size_t random_context_size(void) { return sizeof(uint32_t); }
static void random_init(crypto_random_ctx_t context) { *(uint32_t *)context = 1; }
static void random_generate(crypto_random_ctx_t context, void *output, size_t size) {
    (void)context;
    rng_generate(&rng_state, size, output);
}
static void random_uniform(crypto_random_ctx_t context, uint64_t bound, uint64_t *output) {
    if (!bound) { *output = 0; return; }
    uint64_t value, threshold = -bound % bound;
    do { random_generate(context, &value, sizeof(value)); } while (value < threshold);
    *output = value % bound;
}

static const struct ccdigest_info *digest_info(crypto_digest_alg_t algorithm) {
    switch (algorithm) {
    case CRYPTO_DIGEST_ALG_MD5: return &embedded_md5_di;
    case CRYPTO_DIGEST_ALG_SHA1: return &embedded_sha1_di;
    case CRYPTO_DIGEST_ALG_SHA256: return &ccsha256_ltc_di;
    case CRYPTO_DIGEST_ALG_SHA512: return &ccsha512_ltc_di;
    default: return NULL;
    }
}
static const struct ccdigest_info *require_digest(crypto_digest_alg_t algorithm) {
    const struct ccdigest_info *di = digest_info(algorithm);
    if (!di) panic("embedded crypto: unsupported digest %u", algorithm);
    return di;
}
static void digest_finish(const struct ccdigest_info *di, ccdigest_ctx_t context, void *output) {
    ccdigest_final(di, context, output);
}
static void digest_once(const struct ccdigest_info *di, unsigned long size,
                        const void *data, void *output) {
    ccdigest_di_decl(di, context);
    ccdigest_init(di, context);
    ccdigest_update(di, context, size, data);
    ccdigest_final(di, context, output);
    ccdigest_di_clear(di, context);
}
static size_t digest_context_size(crypto_digest_alg_t algorithm) {
    const struct ccdigest_info *di = digest_info(algorithm);
    return di ? ccdigest_di_size(di) : 0;
}
static void digest_init(crypto_digest_alg_t algorithm, void *context, size_t size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < ccdigest_di_size(di)) panic("embedded crypto: digest context bounds");
    ccdigest_init(di, context);
}
static void digest_update(crypto_digest_alg_t algorithm, void *context, size_t size,
                          const void *data, size_t data_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < ccdigest_di_size(di)) panic("embedded crypto: digest context bounds");
    ccdigest_update(di, context, data_size, data);
}
static void digest_final(crypto_digest_alg_t algorithm, void *context, size_t size,
                         void *output, size_t output_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < ccdigest_di_size(di) || output_size > di->output_size)
        panic("embedded crypto: digest output bounds");
    uint8_t digest[64];
    ccdigest_final(di, context, digest);
    memcpy(output, digest, output_size);
    cc_clear(sizeof(digest), digest);
}
static void digest_generate(crypto_digest_alg_t algorithm, const void *data, size_t size,
                            void *output, size_t output_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (output_size > di->output_size) panic("embedded crypto: digest output bounds");
    uint8_t digest[64];
    digest_once(di, size, data, digest);
    memcpy(output, digest, output_size);
    cc_clear(sizeof(digest), digest);
}

static size_t hmac_context_size(crypto_digest_alg_t algorithm) {
    const struct ccdigest_info *di = digest_info(algorithm);
    return di ? cchmac_di_size(di) : 0;
}
static void hmac_init(crypto_digest_alg_t algorithm, void *context, size_t size,
                      const void *key, size_t key_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < cchmac_di_size(di)) panic("embedded crypto: HMAC context bounds");
    cchmac_init(di, context, key_size, key);
}
static void hmac_update(crypto_digest_alg_t algorithm, void *context, size_t size,
                        const void *data, size_t data_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < cchmac_di_size(di)) panic("embedded crypto: HMAC context bounds");
    cchmac_update(di, context, data_size, data);
}
static void hmac_final(crypto_digest_alg_t algorithm, void *context, size_t size,
                       void *output, size_t output_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (size < cchmac_di_size(di) || output_size > di->output_size)
        panic("embedded crypto: HMAC output bounds");
    uint8_t mac[64];
    cchmac_final(di, context, mac);
    memcpy(output, mac, output_size);
    cc_clear(sizeof(mac), mac);
}
static bool hmac_final_verify(crypto_digest_alg_t algorithm, void *context, size_t size,
                              const void *tag, size_t tag_size) {
    uint8_t expected[64];
    if (!tag_size || tag_size > require_digest(algorithm)->output_size) return false;
    hmac_final(algorithm, context, size, expected, tag_size);
    bool valid = cc_cmp_safe(tag_size, expected, tag) == 0;
    cc_clear(sizeof(expected), expected);
    return valid;
}
static void hmac_generate(crypto_digest_alg_t algorithm, const void *key, size_t key_size,
                          const void *data, size_t size, void *tag, size_t tag_size) {
    const struct ccdigest_info *di = require_digest(algorithm);
    if (tag_size > di->output_size) panic("embedded crypto: HMAC output bounds");
    uint8_t mac[64];
    cchmac(di, key_size, key, size, data, mac);
    memcpy(tag, mac, tag_size);
    cc_clear(sizeof(mac), mac);
}
static bool hmac_verify(crypto_digest_alg_t algorithm, const void *key, size_t key_size,
                        const void *data, size_t size, const void *tag, size_t tag_size) {
    uint8_t expected[64];
    if (!tag_size || tag_size > require_digest(algorithm)->output_size) return false;
    hmac_generate(algorithm, key, key_size, data, size, expected, tag_size);
    bool valid = cc_cmp_safe(tag_size, expected, tag) == 0;
    cc_clear(sizeof(expected), expected);
    return valid;
}

static const struct crypto_functions crypto_functions = {
    .ccdigest_init_fn = ccdigest_init, .ccdigest_update_fn = ccdigest_update,
    .ccdigest_final_fn = digest_finish, .ccdigest_fn = digest_once,
    .ccmd5_di = &embedded_md5_di, .ccsha1_di = &embedded_sha1_di,
    .ccsha256_di = &ccsha256_ltc_di, .ccsha512_di = &ccsha512_ltc_di,
    .cchmac_init_fn = cchmac_init, .cchmac_update_fn = cchmac_update,
    .cchmac_final_fn = cchmac_final, .cchmac_fn = cchmac,
    .ccaes_cbc_encrypt = &embedded_aes_cbc_encrypt,
    .ccaes_cbc_decrypt = &embedded_aes_cbc_decrypt,
    .ccrng_fn = rng_get,
    .random_generate_fn = random_generate, .random_uniform_fn = random_uniform,
    .random_kmem_ctx_size_fn = random_context_size, .random_kmem_init_fn = random_init,
    .digest_ctx_size_fn = digest_context_size, .digest_init_fn = digest_init,
    .digest_update_fn = digest_update, .digest_final_fn = digest_final,
    .digest_fn = digest_generate,
    .hmac_ctx_size_fn = hmac_context_size, .hmac_init_fn = hmac_init,
    .hmac_update_fn = hmac_update, .hmac_final_generate_fn = hmac_final,
    .hmac_final_verify_fn = hmac_final_verify,
    .hmac_generate_fn = hmac_generate, .hmac_verify_fn = hmac_verify,
};

static void crypto_selftest(void) {
    uint8_t output[128], key[20];
    memset(key, 11, sizeof(key));
    digest_once(&ccsha256_ltc_di, 3, "abc", output);
    if (cc_cmp_safe(sizeof(kat_sha256), kat_sha256, output))
        panic("embedded crypto: SHA-256 self-test failed");
    digest_once(&ccsha512_ltc_di, 3, "abc", output);
    if (cc_cmp_safe(sizeof(kat_sha512), kat_sha512, output))
        panic("embedded crypto: SHA-512 self-test failed");
    cchmac(&ccsha256_ltc_di, sizeof(key), key, 8, "Hi There", output);
    if (cc_cmp_safe(sizeof(kat_hmac), kat_hmac, output))
        panic("embedded crypto: HMAC self-test failed");
    struct ccdrbg_info info;
    const struct ccdrbg_nisthmac_custom custom = {.di = &ccsha256_ltc_di, .strictFIPS = 0};
    uint8_t state[CCKPRNG_DRBG_STATE_MAX_SIZE] __attribute__((aligned(16)));
    ccdrbg_factory_nisthmac(&info, &custom);
    if (info.size > sizeof(state)) panic("embedded crypto: self-test state bounds");
    check_drbg(ccdrbg_init(&info, (struct ccdrbg_state *)state,
                          sizeof(kat_entropy), kat_entropy, sizeof(kat_nonce), kat_nonce, 0, NULL));
    check_drbg(ccdrbg_generate(&info, (struct ccdrbg_state *)state, sizeof(output), output, 0, NULL));
    check_drbg(ccdrbg_generate(&info, (struct ccdrbg_state *)state, sizeof(output), output, 0, NULL));
    if (cc_cmp_safe(sizeof(kat_random), kat_random, output))
        panic("embedded crypto: HMAC-DRBG self-test failed");
    ccdrbg_done(&info, (struct ccdrbg_state *)state);
    cc_clear(sizeof(state), state);
    cc_clear(sizeof(output), output);
    cc_clear(sizeof(key), key);
    printf("Embedded crypto known-answer tests passed\n");
}

kern_return_t embedded_crypto_start(kmod_info_t *module, void *data) {
    (void)module; (void)data;
    crypto_selftest();
    embedded_aes_selftest();
    embedded_legacy_digest_selftest();
    if (register_crypto_functions((crypto_functions_t)&crypto_functions)) return KERN_FAILURE;
    register_and_init_prng((struct cckprng_ctx *)&generator, &prng_functions);
    printf("Embedded SHA-2/HMAC-DRBG provider initialized\n");
    return KERN_SUCCESS;
}
kern_return_t embedded_crypto_stop(kmod_info_t *module, void *data) {
    (void)module; (void)data;
    return KERN_FAILURE;
}
KMOD_EXPLICIT_DECL(com.apple.kec.corecrypto, "25.5.0", embedded_crypto_start, embedded_crypto_stop)
