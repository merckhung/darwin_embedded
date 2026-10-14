/* Known-answer checks for the real XNU portable SHA-256 implementation. */
#include <corecrypto/cc.h>
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
#include <stddef.h>

static unsigned nibble(char c)
{
    return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);
}

int native_crypto_check(void)
{
    static const struct {
        const char *input;
        size_t size;
        const char *sha256;
    } vectors[] = {
        {"", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    const struct ccdigest_info *di = ccsha256_di();
    if (!di || di->output_size != 32 || di->block_size != 64) return 40;
    ccdigest_di_decl(di, context);
    unsigned char result[32];
    for (size_t v = 0; v != sizeof(vectors) / sizeof(vectors[0]); ++v) {
        ccdigest_init(di, context);
        /* Deliberately cross partial-buffer paths with split updates. */
        size_t first = vectors[v].size / 3;
        ccdigest_update(di, context, first, vectors[v].input);
        ccdigest_update(di, context, vectors[v].size - first, vectors[v].input + first);
        ccdigest_final(di, context, result);
        for (size_t i = 0; i != sizeof(result); ++i)
            if (result[i] != (nibble(vectors[v].sha256[2 * i]) << 4 |
                              nibble(vectors[v].sha256[2 * i + 1])))
                return 41 + (int)v;
    }
    cc_clear(sizeof(result), result);
    for (size_t i = 0; i != sizeof(result); ++i)
        if (result[i]) return 44;
    return 0;
}
