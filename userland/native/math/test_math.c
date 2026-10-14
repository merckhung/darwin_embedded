/* Native guest regression probe. All computations use the linked math provider;
 * there are no host-computed expected pixels, syscall mocks or replacement math.
 * Returns zero on success, otherwise the numbered failed public-API check. */
#pragma STDC FENV_ACCESS ON
#define __fenv_static
#include <math.h>
#include <fenv.h>
#include <complex.h>

/* Taking addresses requires the actual externally emitted fenv definitions,
 * including the ARM64 feholdexcept implementation that must clear FPSR flags. */
static int (*volatile hold_environment)(fenv_t *) = feholdexcept;
static int (*volatile update_environment)(const fenv_t *) = feupdateenv;
static int (*volatile get_rounding)(void) = fegetround;
static int (*volatile set_rounding)(int) = fesetround;
static int (*volatile set_environment)(const fenv_t *) = fesetenv;

int native_math_probe(void)
{
    fenv_t initial, held;
    if (fegetenv(&initial) != 0 || fesetenv(FE_DFL_ENV) != 0 ||
        set_environment(FE_DFL_ENV) != 0)
        return 1;
#define CHECK(number, condition) do { \
    if (!(condition)) { fesetenv(&initial); return number; } \
} while (0)
    volatile double twelve_squared = 144.0, three = 3.0, two = 2.0, ten = 10.0;
    volatile double sixth_pi = 0.52359877559829887307710723054658;
    CHECK(2, sqrt(twelve_squared) == 12.0);
    CHECK(3, fabs(sin(sixth_pi) - 0.5) < 1e-14);
    CHECK(4, fabs(cos(sixth_pi) - 0.86602540378443864676) < 1e-14);
    CHECK(5, pow(two, ten) == 1024.0);
    CHECK(6, fabs(exp(log(three)) - three) < 1e-14);
    CHECK(7, hypot(three, 4.0) == 5.0);
    volatile float square = 81.0f;
    CHECK(8, sqrtf(square) == 9.0f);
    volatile long double long_square = 16.0L, long_two = 2.0L, long_ten = 10.0L;
    CHECK(9, sqrtl(long_square) == 4.0L);
    CHECK(10, powl(long_two, long_ten) == 1024.0L);
    volatile double complex input = 3.0 + 4.0 * I;
    double complex output = csqrt(input);
    CHECK(11, creal(output) == 2.0 && cimag(output) == 1.0);
    int exponent;
    CHECK(12, frexp(twelve_squared, &exponent) == 0.5625 && exponent == 8);
    volatile double negative_zero = -0.0;
    CHECK(13, sqrt(negative_zero) == 0.0 && signbit(sqrt(negative_zero)));

    CHECK(20, feclearexcept(FE_ALL_EXCEPT) == 0);
    volatile double negative_one = -1.0;
    volatile double invalid = sqrt(negative_one);
    CHECK(21, isnan(invalid));
    CHECK(22, fetestexcept(FE_INVALID) == FE_INVALID);
    CHECK(23, feclearexcept(FE_ALL_EXCEPT) == 0);
    CHECK(24, feraiseexcept(FE_DIVBYZERO | FE_INEXACT) == 0);
    CHECK(25, hold_environment(&held) == 0);
    CHECK(26, fetestexcept(FE_ALL_EXCEPT) == 0);
    CHECK(27, set_rounding(FE_DOWNWARD) == 0 && get_rounding() == FE_DOWNWARD);
    volatile double fractional = 1.75;
    CHECK(28, nearbyint(fractional) == 1.0);
    CHECK(29, set_rounding(FE_UPWARD) == 0 && nearbyint(fractional) == 2.0);
    CHECK(34, feraiseexcept(FE_UNDERFLOW) == 0);
    CHECK(30, update_environment(&held) == 0 && get_rounding() == FE_TONEAREST);
    CHECK(31, fetestexcept(FE_DIVBYZERO | FE_INEXACT | FE_UNDERFLOW) ==
              (FE_DIVBYZERO | FE_INEXACT | FE_UNDERFLOW));
    CHECK(35, update_environment(FE_DFL_ENV) == 0 &&
              fetestexcept(FE_DIVBYZERO | FE_INEXACT | FE_UNDERFLOW) ==
              (FE_DIVBYZERO | FE_INEXACT | FE_UNDERFLOW));
    CHECK(32, set_rounding(-1) != 0 && get_rounding() == FE_TONEAREST);
    CHECK(33, fesetenv(&initial) == 0);
#undef CHECK
    return 0;
}
