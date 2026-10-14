/* Legacy digest adapters for HFS UUIDs and the existing kernel ABI. */
#include <sys/systm.h>
#include <corecrypto/cc_priv.h>
#include "bearssl_hash.h"
#include "embedded_legacy_digest.h"

/* BearSSL internal compression entry points, declared in its src/inner.h. */
extern void br_md5_round(const unsigned char *, uint32_t *);
extern void br_sha1_round(const unsigned char *, uint32_t *);

static void md5_compress(ccdigest_state_t state, size_t blocks, const void *data) {
    const uint8_t *bytes = data;
    while (blocks--) { br_md5_round(bytes, ccdigest_u32(state)); bytes += 64; }
}
static void sha1_compress(ccdigest_state_t state, size_t blocks, const void *data) {
    const uint8_t *bytes = data;
    while (blocks--) { br_sha1_round(bytes, ccdigest_u32(state)); bytes += 64; }
}
static void md5_final(const struct ccdigest_info *di, ccdigest_ctx_t context, unsigned char *output) {
    br_md5_context state = {0};
    unsigned remaining = ccdigest_num(di, context);
    if (remaining >= 64) panic("embedded crypto: MD5 context bounds");
    state.count = ccdigest_nbits(di, context) / 8 + remaining;
    memcpy(state.val, ccdigest_state_u32(di, context), sizeof(state.val));
    memcpy(state.buf, ccdigest_data(di, context), remaining);
    br_md5_out(&state, output);
    cc_clear(sizeof(state), &state);
}
static void sha1_final(const struct ccdigest_info *di, ccdigest_ctx_t context, unsigned char *output) {
    br_sha1_context state = {0};
    unsigned remaining = ccdigest_num(di, context);
    if (remaining >= 64) panic("embedded crypto: SHA-1 context bounds");
    state.count = ccdigest_nbits(di, context) / 8 + remaining;
    memcpy(state.val, ccdigest_state_u32(di, context), sizeof(state.val));
    memcpy(state.buf, ccdigest_data(di, context), remaining);
    br_sha1_out(&state, output);
    cc_clear(sizeof(state), &state);
}
static const uint32_t initial_md5[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
static const uint32_t initial_sha1[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
static const unsigned char oid_md5[] = {0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05};
static const unsigned char oid_sha1[] = {0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a};
const struct ccdigest_info embedded_md5_di = {
    .output_size = 16, .state_size = sizeof(initial_md5), .block_size = 64,
    .oid = oid_md5, .oid_size = sizeof(oid_md5), .initial_state = initial_md5,
    .compress = md5_compress, .final = md5_final,
};
const struct ccdigest_info embedded_sha1_di = {
    .output_size = 20, .state_size = sizeof(initial_sha1), .block_size = 64,
    .oid = oid_sha1, .oid_size = sizeof(oid_sha1), .initial_state = initial_sha1,
    .compress = sha1_compress, .final = sha1_final,
};

void embedded_legacy_digest_selftest(void) {
    /* Public numeric RFC 1321 / FIPS 180 known-answer values for "abc". */
    static const uint8_t md5[] = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    static const uint8_t sha1[] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    uint8_t output[20];
    ccdigest_di_decl(&embedded_sha1_di, context);
    ccdigest_init(&embedded_md5_di, context);
    ccdigest_update(&embedded_md5_di, context, 3, "abc");
    md5_final(&embedded_md5_di, context, output);
    if (cc_cmp_safe(sizeof(md5), md5, output)) panic("embedded crypto: MD5 self-test failed");
    ccdigest_init(&embedded_sha1_di, context);
    ccdigest_update(&embedded_sha1_di, context, 1, "a");
    ccdigest_update(&embedded_sha1_di, context, 2, "bc");
    sha1_final(&embedded_sha1_di, context, output);
    if (cc_cmp_safe(sizeof(sha1), sha1, output)) panic("embedded crypto: SHA-1 self-test failed");
    /* RFC 1321's 80-byte case exercises compression plus a partial final block. */
    static const char long_message[] = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    static const uint8_t long_md5[] = {0x57,0xed,0xf4,0xa2,0x2b,0xe3,0xc9,0x55,0xac,0x49,0xda,0x2e,0x21,0x07,0xb6,0x7a};
    static const uint8_t long_sha1[] = {0x50,0xab,0xf5,0x70,0x6a,0x15,0x09,0x90,0xa0,0x8b,0x2c,0x5e,0xa4,0x0f,0xa0,0xe5,0x85,0x55,0x47,0x32};
    ccdigest_init(&embedded_md5_di, context);
    ccdigest_update(&embedded_md5_di, context, 80, long_message);
    md5_final(&embedded_md5_di, context, output);
    if (cc_cmp_safe(sizeof(long_md5), long_md5, output)) panic("embedded crypto: MD5 multi-block self-test failed");
    ccdigest_init(&embedded_sha1_di, context);
    ccdigest_update(&embedded_sha1_di, context, 13, long_message);
    ccdigest_update(&embedded_sha1_di, context, 67, long_message + 13);
    sha1_final(&embedded_sha1_di, context, output);
    if (cc_cmp_safe(sizeof(long_sha1), long_sha1, output)) panic("embedded crypto: SHA-1 multi-block self-test failed");
    ccdigest_di_clear(&embedded_sha1_di, context);
    cc_clear(sizeof(output), output);
    printf("Embedded MD5/SHA-1 compatibility known-answer tests passed\n");
}
