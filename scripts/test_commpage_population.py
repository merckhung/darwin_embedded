"""Inject a timer update during the actual XNU commpage population function.

The host models a read-only alias until the first population write. Early
pointer publication then faults, reproducing the observed QEMU boot panic.
This checks initialization ordering; native boot checks cover the real VM.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/Kernel/xnu/12377.121.6/osfmk/i386/commpage/commpage.c"


def function(source, name):
    start = re.search(r"^" + re.escape(name) + r"\(", source, re.MULTILINE).start()
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class CommpageTests(unittest.TestCase):
    def test_timer_cannot_write_an_uninitialized_32_or_64_bit_page(self):
        source = SOURCE.read_text()
        address = function(source, "commpage_addr_of")
        populate = function(source, "commpage_populate_one")
        update = function(source, "commpage_update_mach_approximate_time")
        # Only ordering is under test; synthetic offsets fit within one host page.
        offsets = {"_COMM_PAGE_APPROX_TIME": 128, "_COMM_PAGE_END": 4096,
                   "_COMM_PAGE_SIGNATURELEN": 16}
        names = set(re.findall(r"\b_COMM_PAGE[A-Z0-9_]+\b", address + populate + update))
        definitions = "\n".join(f"#define {name} {offsets.get(name, 0)}" for name in sorted(names))
        program = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#define CONFIG_MACH_APPROXIMATE_TIME 1
#define kCache64 1
#define kCache128 2
#define PAGE_SHIFT 12
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define panic(...) abort()
typedef void *vm_map_t;
typedef size_t vm_size_t;
typedef int vm_prot_t;
typedef uint32_t commpage_address_t;
typedef uint64_t commpage_time_data, new_commpage_timeofday_data_t;
static char *commPagePtr, *commPagePtr32, *commPagePtr64, *allocation;
static commpage_address_t commPageBaseOffset, next;
static uint64_t _cpu_capabilities;
static int writes;
static struct { int physical_cpu_max, logical_cpu_max; } machine_info = {1, 1};
static struct cpu_info { uint32_t cpuid_cpufamily; } cpu;
static struct cpu_info *cpuid_info(void) { return &cpu; }
static int ml_get_cluster_count(void) { return 1; }
static uint64_t ml_cpu_cache_size(int ignored) { (void)ignored; return 4096; }
static void ml_map_cpus_to_clusters(uint8_t *map) { memset(map, 0, 256); }
static void commpage_update_mach_approximate_time(uint64_t);
static void *commpage_allocate(vm_map_t map, vm_size_t size, vm_prot_t permission) {
    (void)map; (void)permission;
    allocation = mmap(NULL, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(allocation != MAP_FAILED);
    return allocation;
}
static void commpage_stuff(uint32_t offset, const void *data, size_t size) {
    /* Interrupt before resolving the initial write fault. */
    commpage_update_mach_approximate_time(1);
    if (!writes++) assert(mprotect(allocation, 4096, PROT_READ | PROT_WRITE) == 0);
    memcpy(allocation + offset, data, size);
}
''' + definitions + "\nstatic void *\n" + address + "\nstatic void\n" + populate + \
            "\nstatic void\n" + update + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    struct rlimit core = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &core) == 0);
    char **published = atoi(argv[1]) == 32 ? &commPagePtr32 : &commPagePtr64;
    commpage_time_data *time = NULL;
    new_commpage_timeofday_data_t *gtod = NULL;
    commpage_populate_one(NULL, published, 4096, 0, &time, &gtod, "native", 1);
    assert(writes > 0 && *published == allocation && time && gtod);
    assert(*(uint64_t *)(allocation + _COMM_PAGE_APPROX_TIME) == 0);
    commpage_update_mach_approximate_time(99);
    assert(*(uint64_t *)(allocation + _COMM_PAGE_APPROX_TIME) == 99);
    assert(munmap(allocation, 4096) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            code, binary = work / "population.c", work / "population"
            code.write_text(program)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", str(code), "-o", str(binary)],
                           check=True)
            for bits in (32, 64):
                with self.subTest(bits=bits):
                    subprocess.run([str(binary), str(bits)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
