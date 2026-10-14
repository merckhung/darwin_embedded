#include <stdint.h>
#include <mach-o/loader.h>
#include <mach-o/arm/reloc.h>
#include <stuff/bool.h>
#include <stuff/bytesex.h>

void swap_arm_thread_state_t(arm_thread_state_t *cpu, enum byte_sex target_byte_sex) { return; }
void swap_arm_thread_state64_t(arm_thread_state64_t *cpu, enum byte_sex target_byte_sex) { return; }
void swap_arm_exception_state64_t(arm_exception_state64_t *except, enum byte_sex target_byte_sex) { return; }
