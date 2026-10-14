#include "efi_emulation.h"
#include "serial.h"

#if defined(__x86_64__)
/* Frame pushed by exception_common, lowest address first. */
typedef struct {
  UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
  UINT64 rdi, rsi, rbp, rbx, rdx, rcx, rax;
  UINT64 vector, error;
  UINT64 rip, cs, rflags, rsp, ss;
} ExceptionFrame;

typedef struct __attribute__((packed)) {
  UINT16 offset_low;
  UINT16 selector;
  UINT8 ist;
  UINT8 type_attr;
  UINT16 offset_mid;
  UINT32 offset_high;
  UINT32 zero;
} IdtEntry;

typedef struct __attribute__((packed)) {
  UINT16 limit;
  UINT64 base;
} IdtPointer;

extern const UINT64 efiemu_exception_stubs[32];
static IdtEntry idt[32];

static void out_e9(char c) {
  __asm__ volatile("outb %0, $0xe9" : : "a"(c));
}
#endif

void efiemu_debug_string(const char *s) {
#if defined(__x86_64__)
  for (const char *p = s; *p; ++p)
    out_e9(*p);
#endif
  serial_puts8((CONST CHAR8 *)s);
}

void efiemu_debug_hex(UINT64 value) {
  char text[19] = "0x";
  for (int i = 0; i < 16; ++i) {
    UINT8 nibble = (value >> ((15 - i) * 4)) & 0xf;
    text[2 + i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
  }
  text[18] = 0;
  efiemu_debug_string(text);
}

#if defined(__aarch64__)
extern const UINT8 efiemu_arm64_vectors[];

void efiemu_exceptions_install(void) {
  __asm__ volatile("msr vbar_el1, %0; isb" : : "r"(efiemu_arm64_vectors));
}

static void report(const char *name, UINT64 value) {
  efiemu_debug_string(name);
  efiemu_debug_hex(value);
  efiemu_debug_string("\n");
}

/* Called from the vector stub with x0 = vector slot, x1 = saved x0..x30 */
void efiemu_arm64_exception(UINT64 slot, UINT64 *regs) {
  UINT64 esr, elr, far, spsr;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
  __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
  __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  __asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
  efiemu_debug_string("\nefi-emulation: CPU exception\n");
  report("  slot ", slot);
  report("  esr  ", esr);
  report("  elr  ", elr);
  report("  far  ", far);
  report("  spsr ", spsr);
  report("  x0   ", regs[0]);
  report("  x1   ", regs[1]);
  report("  lr   ", regs[30]);
  for (;;)
    __asm__ volatile("msr daifset, #0xf; wfi");
}
#elif defined(__riscv)
extern const UINT8 efiemu_riscv64_trap[];

void efiemu_exceptions_install(void) {
  __asm__ volatile("csrw stvec, %0" : : "r"(efiemu_riscv64_trap));
}

static void report(const char *name, UINT64 value) {
  efiemu_debug_string(name);
  efiemu_debug_hex(value);
  efiemu_debug_string("\n");
}

// called from the trap stub with x0..x31 saved, regs[2] is the sp at the trap
void efiemu_riscv64_exception(UINT64 *regs) {
  UINT64 scause, sepc, stval, sstatus;
  __asm__ volatile("csrr %0, scause" : "=r"(scause));
  __asm__ volatile("csrr %0, sepc" : "=r"(sepc));
  __asm__ volatile("csrr %0, stval" : "=r"(stval));
  __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
  efiemu_debug_string("\nefi-emulation: CPU exception\n");
  report("  scause  ", scause);
  report("  sepc    ", sepc);
  report("  stval   ", stval);
  report("  sstatus ", sstatus);
  report("  ra      ", regs[1]);
  report("  sp      ", regs[2]);
  report("  a0      ", regs[10]);
  report("  a1      ", regs[11]);
  for (;;)
    __asm__ volatile("csrw sie, zero; wfi");
}
#else
void efiemu_exceptions_install(void) {
  for (UINTN i = 0; i < 32; ++i) {
    UINT64 handler = efiemu_exception_stubs[i];
    idt[i].offset_low = handler & 0xffff;
    idt[i].selector = 0x08;
    idt[i].ist = 0;
    idt[i].type_attr = 0x8e; /* present, ring 0, interrupt gate */
    idt[i].offset_mid = (handler >> 16) & 0xffff;
    idt[i].offset_high = handler >> 32;
    idt[i].zero = 0;
  }
  IdtPointer pointer = {sizeof(idt) - 1, (UINT64)(UINTN)idt};
  __asm__ volatile("lidt (%0)" : : "r"(&pointer));
}

static void report(const char *name, UINT64 value) {
  efiemu_debug_string(name);
  efiemu_debug_hex(value);
  efiemu_debug_string("\n");
}

void efiemu_exception_report(ExceptionFrame *frame) {
  UINT64 cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
  serial_reinit();
  efiemu_debug_string("\nefi-emulation: CPU exception\n");
  report("  vector ", frame->vector);
  report("  error  ", frame->error);
  report("  rip    ", frame->rip);
  report("  rsp    ", frame->rsp);
  report("  rflags ", frame->rflags);
  report("  cr2    ", cr2);
  report("  rax    ", frame->rax);
  report("  rdi    ", frame->rdi);
  report("  rsi    ", frame->rsi);
}
#endif
