// risc-v linux image protocol: the fdt becomes EfiEmuBootInfo, then the efi emulation runs
// the loader runs bare with satp = 0, so there is no page table to build here
#include "efi_emulation.h"
#include "serial.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4

#define MAX_RAM 16
#define MAX_RESERVED 32

struct range {
  UINT64 base, size;
};

static EfiEmuBootInfo boot_info;
static CHAR8 cmdline[2048];
static struct range ram[MAX_RAM], reserved[MAX_RESERVED];
static UINT32 nram, nreserved;
static UINT64 fdt_initrd_start, fdt_initrd_end;

UINT64 efiemu_riscv_hartid;
UINT64 efiemu_riscv_timebase;

// a simple-framebuffer node, committed when the node ends
static struct {
  BOOLEAN compat, disabled, argb;
  UINT64 base;
  UINT32 width, height, stride;
} sfb;

extern CONST CHAR8 embedded_initrd_start[], embedded_initrd_end[];

static UINT32 be32(CONST UINT8 *p) {
  return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static UINT64 cells(CONST UINT8 *p, UINT32 n) {
  UINT64 v = 0;
  for (UINT32 i = 0; i < n; ++i)
    v = v << 32 | be32(p + 4 * i);
  return v;
}

static BOOLEAN str_eq(CONST CHAR8 *a, CONST CHAR8 *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// matches "memory" and "memory@..."
static BOOLEAN node_is(CONST CHAR8 *name, CONST CHAR8 *want) {
  while (*want && *name == *want) {
    ++name;
    ++want;
  }
  return !*want && (!*name || *name == '@');
}

static BOOLEAN compat_has(CONST CHAR8 *v, UINT32 len, CONST CHAR8 *want) {
  UINT32 o = 0;
  while (o < len) {
    if (str_eq(v + o, want))
      return TRUE;
    while (o < len && v[o])
      ++o;
    ++o;
  }
  return FALSE;
}

static void commit_simplefb(void) {
  if (sfb.compat && !sfb.disabled && sfb.argb && sfb.base && sfb.width &&
      sfb.height && sfb.stride >= sfb.width * 4) {
    EfiEmuFramebuffer *fb = &boot_info.framebuffer;
    fb->base = sfb.base;
    fb->width = sfb.width;
    fb->height = sfb.height;
    fb->pixels_per_scanline = sfb.stride / 4;
    fb->bits_per_pixel = 32;
    fb->red_position = 16;
    fb->blue_position = 0;
    fb->valid = 1;
  }
  UINT8 *z = (UINT8 *)&sfb;
  for (UINTN i = 0; i < sizeof(sfb); ++i)
    z[i] = 0;
}

static void add_reserved(UINT64 base, UINT64 size) {
  if (size && nreserved < MAX_RESERVED) {
    reserved[nreserved].base = base;
    reserved[nreserved++].size = size;
  }
}

static void log_ranges(CONST CHAR8 *what, CONST struct range *r, UINT32 n) {
  for (UINT32 i = 0; i < n; ++i) {
    efiemu_debug_string("xnu-loader kernel: ");
    efiemu_debug_string(what);
    efiemu_debug_string(" ");
    efiemu_debug_hex(r[i].base);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(r[i].size);
    efiemu_debug_string("\n");
  }
}

static void parse_fdt(UINT64 fdt) {
  CONST UINT8 *h = (CONST UINT8 *)(UINTN)fdt;
  CONST UINT8 *st = h + be32(h + 8);
  CONST CHAR8 *strings = (CONST CHAR8 *)(h + be32(h + 12));
  CONST UINT8 *rsv = h + be32(h + 16);
  UINT32 addr_cells = 2, size_cells = 1, rsv_addr = 2, rsv_size = 1;
  UINT32 depth = 0;
  // what the node at depth 2 is
  enum { N_OTHER, N_MEMORY, N_CHOSEN, N_CPUS, N_RESMEM } kind = N_OTHER;

  boot_info.fdt = fdt;
  boot_info.fdt_size = be32(h + 4);
  for (;; rsv += 16) {
    UINT64 b = cells(rsv, 2), s = cells(rsv + 8, 2);
    if (!b && !s)
      break;
    add_reserved(b, s);
  }

  for (;;) {
    UINT32 tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      CONST CHAR8 *name = (CONST CHAR8 *)st;
      UINTN len = 0;
      while (name[len])
        ++len;
      st += (len + 4) & ~3UL;
      ++depth;
      if (depth == 2)
        kind = node_is(name, "memory")            ? N_MEMORY
               : node_is(name, "chosen")          ? N_CHOSEN
               : node_is(name, "cpus")            ? N_CPUS
               : node_is(name, "reserved-memory") ? N_RESMEM
                                                  : N_OTHER;
    } else if (tok == FDT_END_NODE) {
      if (depth == 2) {
        commit_simplefb();
        kind = N_OTHER;
      }
      --depth;
    } else if (tok == FDT_PROP) {
      UINT32 len = be32(st);
      CONST CHAR8 *pname = strings + be32(st + 4);
      CONST UINT8 *v = st + 8;
      st += 8 + ((len + 3) & ~3U);
      if (depth == 1) {
        if (str_eq(pname, "#address-cells"))
          addr_cells = rsv_addr = be32(v);
        else if (str_eq(pname, "#size-cells"))
          size_cells = rsv_size = be32(v);
      } else if (depth == 2 && kind == N_MEMORY && str_eq(pname, "reg")) {
        UINT32 stride = 4 * (addr_cells + size_cells);
        for (UINT32 o = 0; o + stride <= len && nram < MAX_RAM; o += stride) {
          ram[nram].base = cells(v + o, addr_cells);
          ram[nram++].size = cells(v + o + 4 * addr_cells, size_cells);
        }
      } else if (depth == 2 && kind == N_CHOSEN) {
        if (str_eq(pname, "bootargs")) {
          UINT32 n = len < sizeof(cmdline) ? len : sizeof(cmdline) - 1;
          for (UINT32 i = 0; i < n; ++i)
            cmdline[i] = (CHAR8)v[i];
          cmdline[n] = 0;
        } else if (str_eq(pname, "linux,initrd-start")) {
          fdt_initrd_start = cells(v, len / 4);
        } else if (str_eq(pname, "linux,initrd-end")) {
          fdt_initrd_end = cells(v, len / 4);
        }
      } else if (depth == 2 && kind == N_CPUS && str_eq(pname, "timebase-frequency")) {
        efiemu_riscv_timebase = cells(v, len / 4);
      } else if (depth == 2 && kind == N_OTHER) {
        if (str_eq(pname, "compatible"))
          sfb.compat = compat_has((CONST CHAR8 *)v, len, "simple-framebuffer");
        else if (str_eq(pname, "status"))
          sfb.disabled = !str_eq((CONST CHAR8 *)v, "okay") && !str_eq((CONST CHAR8 *)v, "ok");
        else if (str_eq(pname, "format"))
          sfb.argb = str_eq((CONST CHAR8 *)v, "a8r8g8b8") || str_eq((CONST CHAR8 *)v, "x8r8g8b8");
        else if (str_eq(pname, "reg") && len >= 4 * addr_cells)
          sfb.base = cells(v, addr_cells);
        else if (str_eq(pname, "width"))
          sfb.width = be32(v);
        else if (str_eq(pname, "height"))
          sfb.height = be32(v);
        else if (str_eq(pname, "stride"))
          sfb.stride = be32(v);
      } else if (depth == 2 && kind == N_RESMEM) {
        if (str_eq(pname, "#address-cells"))
          rsv_addr = be32(v);
        else if (str_eq(pname, "#size-cells"))
          rsv_size = be32(v);
      } else if (depth == 3 && kind == N_RESMEM && str_eq(pname, "reg")) {
        UINT32 stride = 4 * (rsv_addr + rsv_size);
        for (UINT32 o = 0; o + stride <= len; o += stride)
          add_reserved(cells(v + o, rsv_addr), cells(v + o + 4 * rsv_addr, rsv_size));
      }
    } else if (tok == FDT_NOP) {
      continue;
    } else {
      break;
    }
  }
}

// ram minus the reserved ranges, as the usable and reserved list efi-emulation expects
static void build_memory_map(void) {
  for (UINT32 r = 0; r < nram; ++r) {
    UINT64 b = ram[r].base, e = ram[r].base + ram[r].size;
    while (b < e && boot_info.memory_count < EFIEMU_MAX_MEMORY_RANGES - 1) {
      UINT64 cut = e, skip = e;
      for (UINT32 i = 0; i < nreserved; ++i) {
        UINT64 rb = reserved[i].base, re = rb + reserved[i].size;
        if (re > b && rb < cut) {
          cut = rb > b ? rb : b;
          skip = re;
        }
      }
      if (cut > b) {
        EfiEmuMemoryRange *m = &boot_info.memory[boot_info.memory_count++];
        m->base = b;
        m->length = cut - b;
        m->type = EfiEmuMemoryUsable;
      }
      if (skip > cut && boot_info.memory_count < EFIEMU_MAX_MEMORY_RANGES) {
        EfiEmuMemoryRange *m = &boot_info.memory[boot_info.memory_count++];
        m->base = cut;
        m->length = (skip < e ? skip : e) - cut;
        m->type = EfiEmuMemoryReserved;
      }
      b = skip;
    }
  }
}

static void __attribute__((noreturn)) halt(void) {
  for (;;)
    __asm__ volatile("wfi");
}

void kernel_riscv64_main(UINT64 hartid, UINT64 fdt) {
  serial_init();
  efiemu_exceptions_install();
  efiemu_riscv_hartid = hartid;
  efiemu_debug_string("\nxnu-loader kernel: riscv64 Image entry on hart ");
  efiemu_debug_hex(hartid);
  efiemu_debug_string(", fdt ");
  efiemu_debug_hex(fdt);
  efiemu_debug_string("\n");
  if (!fdt || be32((CONST UINT8 *)(UINTN)fdt) != FDT_MAGIC) {
    efiemu_debug_string("xnu-loader kernel: no device tree in a1\n");
    halt();
  }
  parse_fdt(fdt);
  efiemu_debug_string("xnu-loader kernel: device tree parsed, timebase ");
  efiemu_debug_hex(efiemu_riscv_timebase);
  efiemu_debug_string(" Hz\n");
  log_ranges("ram", ram, nram);
  log_ranges("reserved", reserved, nreserved);

  UINT64 initrd = fdt_initrd_start;
  UINT64 initrd_size = fdt_initrd_end > fdt_initrd_start ? fdt_initrd_end - fdt_initrd_start : 0;
  // clang folds a compare of two distinct arrays, so compare the addresses opaquely
  UINTN embedded_start = (UINTN)embedded_initrd_start, embedded_end = (UINTN)embedded_initrd_end;
  __asm__("" : "+r"(embedded_start), "+r"(embedded_end));
  if (embedded_end != embedded_start) {
    initrd = embedded_start;
    initrd_size = embedded_end - embedded_start;
    efiemu_debug_string("xnu-loader kernel: using the built-in initrd\n");
  }
  if (initrd && initrd_size) {
    efiemu_debug_string("xnu-loader kernel: initrd ");
    efiemu_debug_hex(initrd);
    efiemu_debug_string(" size ");
    efiemu_debug_hex(initrd_size);
    efiemu_debug_string("\n");
    kernel_parse_cpio(&boot_info, initrd, initrd_size);
  } else {
    efiemu_debug_string("xnu-loader kernel: no initrd\n");
  }
  build_memory_map();
  if (cmdline[0])
    boot_info.cmdline = cmdline;
  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: device tree has no memory\n");
    halt();
  }
  efiemu_main(&boot_info);
}
