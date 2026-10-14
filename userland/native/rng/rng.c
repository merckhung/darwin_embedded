/* Native ccrng ABI backed by XNU's initialized cryptographic RNG.
 * No userspace seed, mutable shared stream state, or alternate OS ABI.
 */
#include <corecrypto/ccrng.h>
#include <corecrypto/cc_error.h>
#include <stdint.h>

/* Actual entry point from the matching Apple libsyscall archive. */
extern int getentropy(void *, size_t);

static int generate(struct ccrng_state *rng, size_t count, void *output)
{
    (void)rng;
    if (count && !output) return CCERR_PARAMETER;
    unsigned char *cursor = output;
    size_t remaining = count;
    while (remaining) {
        size_t chunk = remaining > 256 ? 256 : remaining;
        if (getentropy(cursor, chunk)) {
            /* Partial output must never look like a successful RNG result. */
            volatile unsigned char *clear = output;
            for (size_t i = 0; i < count; ++i) clear[i] = 0;
            return CCERR_OUT_OF_ENTROPY;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return CCERR_OK;
}

/* XNU owns all RNG state and synchronization. This callback is immutable
 * after relocation, so acquiring a handle requires no lazy initialization. */
static struct ccrng_state system_rng = { .generate = generate };

struct ccrng_state *ccrng(int *error)
{
    if (error) *error = CCERR_OK;
    return &system_rng;
}

int ccrng_uniform(struct ccrng_state *rng, uint64_t bound, uint64_t *output)
{
    if (!rng || !rng->generate || !output || !bound) return CCERR_PARAMETER;
    if (bound == 1) {
        *output = 0;
        return CCERR_OK;
    }
    /* Reject the short prefix of the 2^64 sample space so every residue
     * has exactly the same number of possible accepted inputs. */
    uint64_t threshold = (uint64_t)(-bound) % bound;
    uint64_t value;
    int error;
    do {
        error = ccrng_generate(rng, sizeof(value), &value);
        if (error) return error;
    } while (value < threshold);
    *output = value % bound;
    return CCERR_OK;
}
