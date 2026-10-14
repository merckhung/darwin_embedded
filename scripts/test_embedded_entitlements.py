"""Exercise the actual XNU entitlement acceleration function on a native host.

Only the external AMFI ABI is modeled; the tested policy comes from XNU source.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / "src/Kernel/xnu/12377.121.6"


class EntitlementTests(unittest.TestCase):
    def test_absent_amfi_denies_privilege_and_preserves_provider_errors(self):
        source = (XNU / "bsd/kern/ubc_subr.c").read_text()
        start = source.index("accelerate_entitlement_queries(")
        opening = source.index("{", start)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        program = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include "cs_flags.h"
#define DARWIN_EMBEDDED_NO_APPLE_TRUST_CACHE 1
#define CODE_SIGNING_MONITOR 0
#define KERN_SUCCESS 0
#define KERN_NOT_SUPPORTED 46
typedef int errno_t, kern_return_t;
struct cs_blob {
    uint32_t csb_flags;
    void *csb_entitlements, *csb_entitlements_blob, *csb_der_entitlements_blob;
    int csb_platform_binary, csb_platform_path;
    const char *csb_teamid;
};
struct provider {
    struct {
        int (*adjustContext)(void *, struct cs_blob *, const void *);
        int (*adjustContextWithoutMonitor)(void *, struct cs_blob *);
    } OSEntitlements;
};
static struct provider *amfi;
static int calls, response;
static int adjust(void *entitlements, struct cs_blob *blob, const void *context) {
    (void)entitlements; (void)blob; assert(context == NULL); ++calls; return response;
}
static int legacy_adjust(void *entitlements, struct cs_blob *blob) {
    return adjust(entitlements, blob, NULL);
}
static errno_t
''' + source[start:end] + r'''
int main(void) {
    struct cs_blob empty = { .csb_flags = CS_ADHOC | CS_LINKER_SIGNED };
    assert(accelerate_entitlement_queries(&empty) == 0);
    struct cs_blob b = {0};
    assert(accelerate_entitlement_queries(&b) == EPERM);
    const uint32_t denied[] = {CS_GET_TASK_ALLOW, CS_INSTALLER, CS_DATAVAULT_CONTROLLER,
        CS_NVRAM_UNRESTRICTED, CS_ENTITLEMENTS_VALIDATED, CS_PLATFORM_BINARY, CS_PLATFORM_PATH};
    for (unsigned i = 0; i < sizeof(denied)/sizeof(denied[0]); ++i) {
        b = empty; b.csb_flags |= denied[i];
        assert(accelerate_entitlement_queries(&b) == EPERM);
    }
    b = empty; b.csb_entitlements = &b; assert(accelerate_entitlement_queries(&b) == EPERM);
    b = empty; b.csb_entitlements_blob = &b; assert(accelerate_entitlement_queries(&b) == EPERM);
    b = empty; b.csb_der_entitlements_blob = &b; assert(accelerate_entitlement_queries(&b) == EPERM);
    b = empty; b.csb_platform_binary = 1; assert(accelerate_entitlement_queries(&b) == EPERM);
    b = empty; b.csb_platform_path = 1; assert(accelerate_entitlement_queries(&b) == EPERM);
    b = empty; b.csb_teamid = "claimed-team"; assert(accelerate_entitlement_queries(&b) == EPERM);
    assert(calls == 0);
    struct provider provider = { .OSEntitlements.adjustContext = adjust };
    amfi = &provider; response = KERN_SUCCESS;
    assert(accelerate_entitlement_queries(&empty) == 0 && calls == 1);
    response = KERN_NOT_SUPPORTED;
    assert(accelerate_entitlement_queries(&empty) == EPERM && calls == 2);
    provider.OSEntitlements.adjustContext = NULL;
    provider.OSEntitlements.adjustContextWithoutMonitor = legacy_adjust;
    response = KERN_SUCCESS;
    assert(accelerate_entitlement_queries(&empty) == 0 && calls == 3);
    response = 5;
    assert(accelerate_entitlement_queries(&empty) == EPERM && calls == 4);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            header = (XNU / "osfmk/kern/cs_blobs.h").read_text()
            # Use the actual flag definitions without importing Darwin-only types.
            (work / "cs_flags.h").write_text(header[header.index("/* code signing attributes"):
                                                   header.index("/* executable segment flags */")])
            (work / "entitlements.c").write_text(program)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror",
                            str(work / "entitlements.c"), "-o", str(work / "entitlements")], check=True)
            subprocess.run([str(work / "entitlements")], check=True)


if __name__ == "__main__":
    unittest.main()
