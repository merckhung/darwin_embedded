#ifndef NATIVE_MATH_TARGET_CONFIG_H
#define NATIVE_MATH_TARGET_CONFIG_H

#if !defined(__APPLE__) || !defined(__MACH__)
#error Native math requires the Darwin Mach-O ABI
#endif

/* OpenLibm's portable Clang fallback omits Darwin's leading underscore and
 * global definition visibility. Supply the native object-format operations,
 * retaining the upstream distinction between weak and strong aliases. The
 * aliased implementation remains the real upstream math function. */
#define __weak_reference(symbol, alias) \
    __asm__(".globl _" #alias "\n.weak_definition _" #alias \
            "\n.set _" #alias ", _" #symbol)
#define __strong_reference(symbol, alias) \
    __asm__(".globl _" #alias "\n.set _" #alias ", _" #symbol)

#if defined(__arm64__)
_Static_assert(__LDBL_MANT_DIG__ == 53 && sizeof(long double) == sizeof(double),
               "Darwin ARM64 uses double precision for long double aliases");
#else
_Static_assert(__LDBL_MANT_DIG__ == 64, "Darwin x86_64 requires extended precision");
#endif

#endif
