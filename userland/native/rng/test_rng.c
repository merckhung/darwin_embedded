/* Actual kernel RNG checks and isolated rejection/error-path fixtures. */
#include <corecrypto/ccrng.h>
#include <corecrypto/cc_error.h>
#include <stdint.h>

struct scripted_rng {
    struct ccrng_state base;
    unsigned calls;
};

static int scripted(struct ccrng_state *base, size_t count, void *output)
{
    struct scripted_rng *rng = (void *)base;
    static const uint64_t values[] = {0, 5, 16};
    if (count != sizeof(uint64_t) || rng->calls >= 3) return CCERR_INTERNAL;
    *(uint64_t *)output = values[rng->calls++];
    return CCERR_OK;
}

static int failed(struct ccrng_state *rng, size_t count, void *output)
{
    (void)rng; (void)count; (void)output;
    return CCERR_OUT_OF_ENTROPY;
}

int native_rng_check(void)
{
    int error = CCERR_INTERNAL;
    struct ccrng_state *rng = ccrng(&error);
    if (!rng || !rng->generate || error || ccrng(0) != rng) return 80;
    unsigned char first[513], second[513];
    if (ccrng_generate(rng, sizeof(first), first) ||
            ccrng_generate(rng, sizeof(second), second)) return 81;
    unsigned changed = 0, nonzero = 0;
    for (size_t i = 0; i < sizeof(first); ++i) {
        changed |= first[i] ^ second[i];
        nonzero |= first[i] | second[i];
    }
    if (!changed || !nonzero) return 82;
    if (ccrng_generate(rng, 0, 0)) return 83;
    if (ccrng_generate(rng, 1, 0) != CCERR_PARAMETER) return 89;
    uint64_t value = UINT64_MAX;
    if (ccrng_uniform(rng, 1, &value) || value) return 84;
    if (ccrng_uniform(rng, 0, &value) != CCERR_PARAMETER) return 85;
    const uint64_t bounds[] = {2, 10, 0x100000001ULL, UINT64_MAX};
    for (size_t b = 0; b < sizeof(bounds) / sizeof(bounds[0]); ++b)
        for (unsigned i = 0; i < 32; ++i)
            if (ccrng_uniform(rng, bounds[b], &value) || value >= bounds[b]) return 86;
    struct scripted_rng sequence = {{scripted}, 0};
    if (ccrng_uniform(&sequence.base, 10, &value) || value != 6 || sequence.calls != 3) return 87;
    struct ccrng_state unavailable = {failed};
    value = 42;
    if (ccrng_uniform(&unavailable, 10, &value) != CCERR_OUT_OF_ENTROPY || value != 42) return 88;
    return 0;
}
