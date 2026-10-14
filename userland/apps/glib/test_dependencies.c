/* Execute genuine native GLib prerequisites, including callable FFI closures. */
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <ffi.h>
#include <iconv.h>
#include <libintl.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static int failure(const char *stage, int code)
{
    fprintf(stderr, "Native dependencies failure: %s check %d (errno=%d)\n", stage, code, errno);
    return 1;
}

static int regex(void)
{
    int error;
    PCRE2_SIZE offset;
    const char *pattern = "^(\\p{Greek}+)-([0-9]{2})$";
    const char *subject = "\xce\xbb\xce\xb1-42";
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                    PCRE2_UTF | PCRE2_UCP, &error, &offset, NULL);
    if (!code) return 1;
    pcre2_match_data *match = pcre2_match_data_create_from_pattern(code, NULL);
    if (!match) { pcre2_code_free(code); return 2; }
    int result = pcre2_match(code, (PCRE2_SPTR)subject, strlen(subject), 0, 0, match, NULL);
    PCRE2_SIZE *positions = pcre2_get_ovector_pointer(match);
    int failed = result != 3 || positions[0] != 0 || positions[1] != 7 ||
                 positions[2] != 0 || positions[3] != 4 || positions[4] != 5 || positions[5] != 7;
    if (!failed && pcre2_match(code, (PCRE2_SPTR)"abc-42", 6, 0, 0, match, NULL) != PCRE2_ERROR_NOMATCH)
        failed = 1;
    if (!failed && pcre2_match(code, (PCRE2_SPTR)"\xc0\xaf-42", 5, 0, 0, match, NULL) >= 0)
        failed = 1;
    pcre2_match_data_free(match);
    pcre2_code_free(code);
    if (failed) return 3;
    code = pcre2_compile((PCRE2_SPTR)"(", 1, 0, &error, &offset, NULL);
    if (code) { pcre2_code_free(code); return 4; }
    if (error != PCRE2_ERROR_MISSING_CLOSING_PARENTHESIS || offset != 1) return 5;
    return 0;
}

static int conversions(void)
{
    const char *utf8 = "A\xe2\x82\xac\xf0\x9f\x99\x82";
    const unsigned char expected[] = {0x41,0,0xac,0x20,0x3d,0xd8,0x42,0xde};
    char buffer[32];
    iconv_t converter = iconv_open("UTF-16LE", "UTF-8");
    if (converter == (iconv_t)-1) return 1;
    char *input = (char *)utf8, *output = buffer;
    size_t left = 8, space = sizeof(buffer);
    int failed = iconv(converter, &input, &left, &output, &space) != 0 || left ||
                 sizeof(buffer) - space != sizeof(expected) || memcmp(buffer, expected, sizeof(expected));
    if (iconv_close(converter) || failed) return 2;
    converter = iconv_open("UTF-8", "UTF-16LE");
    if (converter == (iconv_t)-1) return 3;
    input = (char *)expected; output = buffer; left = sizeof(expected); space = sizeof(buffer);
    failed = iconv(converter, &input, &left, &output, &space) != 0 || left ||
             sizeof(buffer) - space != 8 || memcmp(buffer, utf8, 8);
    if (iconv_close(converter) || failed) return 4;
    converter = iconv_open("UTF-16LE", "UTF-8");
    if (converter == (iconv_t)-1) return 5;
    input = (char *)utf8; output = buffer; left = 8; space = 0; errno = 0;
    failed = iconv(converter, &input, &left, &output, &space) != (size_t)-1 ||
             errno != E2BIG || input != utf8 || left != 8;
    input = "\xe2\x82"; output = buffer; left = 2; space = sizeof(buffer); errno = 0;
    if (iconv(converter, &input, &left, &output, &space) != (size_t)-1 || errno != EINVAL || left != 2)
        failed = 1;
    input = "\xc0\xaf"; output = buffer; left = 2; space = sizeof(buffer); errno = 0;
    if (iconv(converter, &input, &left, &output, &space) != (size_t)-1 || errno != EILSEQ || left != 2)
        failed = 1;
    if (iconv_close(converter) || failed) return 6;
    errno = 0;
    converter = iconv_open("no-such-native-encoding", "UTF-8");
    if (converter != (iconv_t)-1) { iconv_close(converter); return 7; }
    return errno == EINVAL ? 0 : 8;
}

struct mixed_result { uint64_t number; double fraction; };
static unsigned call_count;
static struct mixed_result mixed(uint64_t number, double fraction)
{
    ++call_count;
    return (struct mixed_result){number + 9, fraction * 2 + 0.5};
}

static double variadic(int count, ...)
{
    va_list arguments;
    va_start(arguments, count);
    double result = 0;
    for (int i = 0; i < count; ++i) result += va_arg(arguments, double);
    va_end(arguments);
    ++call_count;
    return result;
}

static long double extended(long double number)
{
    ++call_count;
    return number * 1.5L + 0.25L;
}

struct callback_context { double bias; atomic_uint calls; };
static void callback(ffi_cif *cif, void *result, void **arguments, void *opaque)
{
    struct callback_context *context = opaque;
    if (cif->nargs != 2) { *(double *)result = -10000; return; }
    *(double *)result = (double)*(uint64_t *)arguments[0] + *(double *)arguments[1] + context->bias;
    atomic_fetch_add(&context->calls, 1);
}

typedef double (*native_callback)(uint64_t, double);
struct callback_worker { native_callback function; unsigned index; int failed; };
static void *callback_thread(void *opaque)
{
    struct callback_worker *worker = opaque;
    for (unsigned i = 0; i < 64; ++i) {
        uint64_t value = worker->index * 100 + i;
        if (worker->function(value, 0.75) != (double)value + 3.25) worker->failed = 1;
    }
    return NULL;
}

static int foreign_calls(void)
{
    ffi_cif cif;
    ffi_type *fields[] = {&ffi_type_uint64, &ffi_type_double, NULL};
    ffi_type record = {0, 0, FFI_TYPE_STRUCT, fields};
    ffi_type *types[] = {&ffi_type_uint64, &ffi_type_double};
    uint64_t number = 0x1122334455667788ULL;
    double fraction = 1.25;
    void *arguments[] = {&number, &fraction};
    struct mixed_result result = {0};
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &record, types) != FFI_OK) return 1;
    ffi_call(&cif, FFI_FN(mixed), &result, arguments);
    if (record.size != sizeof(result) || result.number != number + 9 || result.fraction != 3.0 || call_count != 1)
        return 2;
    ffi_type *var_types[] = {&ffi_type_sint32, &ffi_type_double, &ffi_type_double, &ffi_type_double};
    int count = 3;
    double values[] = {0.5, 1.25, 2.75}, total = 0;
    void *var_arguments[] = {&count, values, values + 1, values + 2};
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 4, &ffi_type_double, var_types) != FFI_OK) return 3;
    ffi_call(&cif, FFI_FN(variadic), &total, var_arguments);
    if (total != 4.5 || call_count != 2) return 4;
    ffi_type *long_types[] = {&ffi_type_longdouble};
    long double value = 1.5L, long_result = 0;
    void *long_arguments[] = {&value};
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_longdouble, long_types) != FFI_OK) return 5;
    ffi_call(&cif, FFI_FN(extended), &long_result, long_arguments);
    if (long_result != 2.5L || call_count != 3) return 6;
    void *code = NULL;
    ffi_closure *closure = ffi_closure_alloc(sizeof(*closure), &code);
    if (!closure || !code) return 7;
    struct callback_context context = {.bias = 2.5, .calls = 0};
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_double, types) != FFI_OK ||
        ffi_prep_closure_loc(closure, &cif, callback, &context, code) != FFI_OK) {
        ffi_closure_free(closure); return 8;
    }
    native_callback function = (native_callback)code;
    if (function(7, 0.75) != 10.25) { ffi_closure_free(closure); return 9; }
    pthread_t threads[4];
    struct callback_worker workers[4];
    unsigned created = 0;
    for (; created < 4; ++created) {
        workers[created] = (struct callback_worker){function, created, 0};
        if (pthread_create(threads + created, NULL, callback_thread, workers + created)) break;
    }
    int failed = created != 4;
    for (unsigned i = 0; i < created; ++i)
        if (pthread_join(threads[i], NULL) || workers[i].failed) failed = 1;
    if (atomic_load(&context.calls) != 257) failed = 1;
    ffi_closure_free(closure);
    return failed ? 10 : 0;
}

static int catalogs(void)
{
    if (setenv("LC_ALL", "zz_ZZ.UTF-8", 1) || unsetenv("LANGUAGE")) return 1;
    if (!bindtextdomain("native-deps", "/usr/share/locale") ||
        !bind_textdomain_codeset("native-deps", "UTF-8") || !textdomain("native-deps")) return 2;
    if (strcmp(gettext("greeting"), "Darwin \xce\xbb") ||
        strcmp(dgettext("native-deps", "place\004greeting"), "context \xce\xbb") ||
        strcmp(ngettext("apple", "apples", 1), "native apple") ||
        strcmp(ngettext("apple", "apples", 3), "native apples") ||
        strcmp(gettext("no translation"), "no translation")) return 3;
    if (!bind_textdomain_codeset("native-deps", "ISO-8859-1") ||
        strcmp(dgettext("native-deps", "latin"), "salut\xe9")) return 4;
    if (!bindtextdomain("missing-native-domain", "/missing-native-catalogs") ||
        strcmp(dgettext("missing-native-domain", "no translation"), "no translation")) return 5;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 48 || strspn(argv[1], "0123456789abcdef") != 48)
        return failure("nonce", 1);
    fprintf(stderr, "Native dependency stage: PCRE2\n");
    int result = regex();
    if (result) return failure("PCRE2", result);
    fprintf(stderr, "Native dependency stage: iconv\n");
    result = conversions();
    if (result) return failure("iconv", result);
    fprintf(stderr, "Native dependency stage: ffi\n");
    result = foreign_calls();
    if (result) return failure("ffi", result);
    fprintf(stderr, "Native dependency stage: gettext\n");
    result = catalogs();
    if (result) return failure("gettext", result);
    struct utsname identity;
    if (uname(&identity)) return failure("uname", 1);
    printf("NATIVE_DEPENDENCIES_OK:%s:%s:%s\n", argv[1], identity.release, identity.machine);
    return 0;
}
