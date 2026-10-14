"""Compile and exercise XNU's actual zone-budget distributor on the host."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class KallocBudgetTests(unittest.TestCase):
    def test_empty_and_exhausted_signature_budgets(self):
        source = (ROOT / "src/Kernel/xnu/12377.121.6/osfmk/kern/kalloc.c").read_text()
        start = source.index("kalloc_type_distribute_budget(")
        opening = source.index("{", start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        program = '''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define MAX_K_ZONE(x) 8
#define KT_OPTIONS_DEBUG 1
static int kt_options;
static uint16_t
''' + source[start:end] + '''
int main(void) {
    uint16_t frequencies[8] = {0}, zones[8] = {0};
    assert(kalloc_type_distribute_budget(frequencies, zones, 64, 0) == 0);
    frequencies[2] = 1;
    assert(kalloc_type_distribute_budget(frequencies, zones, 64, 1) == 0);
    assert(zones[2] == 1);
    uint32_t state = 12345;
    for (unsigned trial = 0; trial < 10000; ++trial) {
        uint16_t minimum = trial % 3, required = 0, total = 0;
        for (unsigned i = 0; i < 8; ++i) {
            state = state * 1664525u + 1013904223u;
            frequencies[i] = (state >> 24) % 16;
            total += frequencies[i];
            required += frequencies[i] < minimum ? frequencies[i] : minimum;
        }
        uint16_t budget = required + trial % 64;
        uint16_t unused = kalloc_type_distribute_budget(frequencies, zones, budget, minimum);
        unsigned assigned = 0;
        for (unsigned i = 0; i < 8; ++i) {
            assert(zones[i] <= frequencies[i]);
            assigned += zones[i];
        }
        assert(assigned + unused == (budget < total ? budget : total));
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "budget.c").write_text(program)
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Werror", str(work / "budget.c"),
                            "-o", str(work / "budget")], check=True)
            subprocess.run([str(work / "budget")], check=True)


if __name__ == "__main__":
    unittest.main()
