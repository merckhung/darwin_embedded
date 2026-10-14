/* arm64 Linux Image protocol: FDT -> EfiEmuBootInfo, an identity map, then
 * the EFI emulation. Runs with the MMU off until arm64_enable_mmu, so this file
 * is built with -mstrict-align (Device memory faults on unaligned access). */
#include "efi_emulation.h"
#include "serial.h"
#include "platform.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

#define MAX_RESERVED 32

static EfiEmuBootInfo boot_info;
static CHAR8 cmdline[2048];
struct range {
  UINT64 base, size;
};

static struct range ram[16], reserved[MAX_RESERVED], released[4];
static UINT32 nram, nreserved, nreleased;
static UINT64 fdt_initrd_start, fdt_initrd_end;

/* Simple-framebuffer node from u-boot, committed at the node end */
static struct {
  BOOLEAN compat, disabled, argb;
  UINT64 base;
  UINT32 width, height, stride;
} sfb;

extern CONST CHAR8 embedded_initrd_start[], embedded_initrd_end[];
extern UINT8 __kernel_start, __kernel_end;

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

/* "memory" or "memory@..." */
static BOOLEAN node_is(CONST CHAR8 *name, CONST CHAR8 *want) {
  while (*want && *name == *want) {
    ++name;
    ++want;
  }
  return !*want && (!*name || *name == '@');
}

/* True when the nul separated compatible list holds want */
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
  /* What the node at depth 1 is */
  enum { N_OTHER, N_MEMORY, N_CHOSEN, N_PSCI, N_RESMEM } kind = N_OTHER;

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
        kind = node_is(name, "memory")           ? N_MEMORY
               : node_is(name, "chosen")         ? N_CHOSEN
               : node_is(name, "psci")           ? N_PSCI
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
        for (UINT32 o = 0; o + stride <= len && nram < 16; o += stride) {
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
        } else if (str_eq(pname, "pd,released-memory")) {
          // ranges the boot script gave back, a firmware buffer it moved for one
          UINT32 stride = 4 * (addr_cells + size_cells);
          for (UINT32 o = 0; o + stride <= len && nreleased < 4; o += stride) {
            released[nreleased].base = cells(v + o, addr_cells);
            released[nreleased++].size = cells(v + o + 4 * addr_cells, size_cells);
          }
        }
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
      } else if (depth == 2 && kind == N_PSCI && str_eq(pname, "method")) {
        efiemu_psci_conduit = str_eq((CONST CHAR8 *)v, "hvc") ? 1
                              : str_eq((CONST CHAR8 *)v, "smc") ? 2 : 0;
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

  // a reservation the script released goes only on an exact match
  for (UINT32 i = 0; i < nreserved;) {
    BOOLEAN drop = FALSE;
    for (UINT32 j = 0; j < nreleased; ++j)
      drop |= reserved[i].base == released[j].base && reserved[i].size == released[j].size;
    if (drop)
      reserved[i] = reserved[--nreserved];
    else
      ++i;
  }
}

/* RAM minus reserved ranges, as the usable/reserved list efi-emulation expects */
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

/* 4K granule, 39-bit VA: 1 GiB blocks, split into 2 MiB blocks where RAM starts.
 * RAM is Normal write-back, everything else Device-nGnRE and execute-never. */
#define PT_BLOCK 1ULL
#define PT_TABLE 3ULL
#define PT_AF (1ULL << 10)
#define PT_SH_INNER (3ULL << 8)
#define PT_ATTR(n) ((UINT64)(n) << 2)
#define PT_XN (3ULL << 53)
#define MAIR_VALUE 0x00000000000004ff00ULL /* 0 Device-nGnRnE, 1 Normal WB, 2 Device-nGnRE */

static UINT64 l1[512] __attribute__((aligned(4096)));
static UINT64 l2[8][512] __attribute__((aligned(4096)));

static BOOLEAN is_ram(UINT64 b, UINT64 e) {
  for (UINT32 r = 0; r < nram; ++r)
    if (b >= ram[r].base && e <= ram[r].base + ram[r].size)
      return TRUE;
  return FALSE;
}

static BOOLEAN touches_ram(UINT64 b, UINT64 e) {
  for (UINT32 r = 0; r < nram; ++r)
    if (b < ram[r].base + ram[r].size && e > ram[r].base)
      return TRUE;
  return FALSE;
}

static UINT64 block(UINT64 pa, BOOLEAN normal) {
  return pa | PT_BLOCK | PT_AF |
         (normal ? PT_ATTR(1) | PT_SH_INNER : PT_ATTR(2) | PT_XN);
}

static void arm64_enable_mmu(void) {
  UINT32 used = 0;
  for (UINT64 g = 0; g < 512; ++g) {
    UINT64 gb = g << 30;
    if (touches_ram(gb, gb + (1ULL << 30)) && !is_ram(gb, gb + (1ULL << 30)) && used < 8) {
      UINT64 *t = l2[used++];
      for (UINT64 i = 0; i < 512; ++i) {
        UINT64 pa = gb + (i << 21);
        t[i] = block(pa, is_ram(pa, pa + (1ULL << 21)));
      }
      l1[g] = (UINT64)(UINTN)t | PT_TABLE;
    } else {
      l1[g] = block(gb, touches_ram(gb, gb + (1ULL << 30)));
    }
  }
  UINT64 mmfr0, ips;
  __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
  ips = mmfr0 & 7;
  if (ips > 5)
    ips = 5;
  /* T0SZ=25, IRGN0/ORGN0 write-back, inner shareable, 4K, TTBR1 walks off */
  UINT64 tcr = 25 | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (ips << 32);
  /* Written with caches off: drop any stale lines firmware left for our image */
  UINT64 ctr, line;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  line = 4ULL << ((ctr >> 16) & 0xf);
  for (UINT64 a = (UINT64)(UINTN)&__kernel_start & ~(line - 1);
       a < (UINT64)(UINTN)&__kernel_end; a += line)
    __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb sy; ic iallu; dsb ish; isb" : : : "memory");
  __asm__ volatile(
      "msr mair_el1, %0\n"
      "msr tcr_el1, %1\n"
      "msr ttbr0_el1, %2\n"
      "isb\n"
      "tlbi vmalle1\n"
      "dsb ish\n"
      "isb\n"
      "mrs x9, sctlr_el1\n"
      "orr x9, x9, #(1 << 0)\n"
      "orr x9, x9, #(1 << 2)\n"
      "orr x9, x9, #(1 << 12)\n"
      "bic x9, x9, #(1 << 1)\n"
      "msr sctlr_el1, x9\n"
      "isb\n"
      :
      : "r"(MAIR_VALUE), "r"(tcr), "r"((UINT64)(UINTN)l1)
      : "x9", "memory");
}

void kernel_arm64_main(UINT64 fdt) {
  // the console comes from the tree, so read the board before the first message
  fdt_board_parse((CONST VOID *)(UINTN)fdt);
  serial_init();
  serial_puts8((CONST CHAR8 *)"\nxnu-loader kernel: C entry\n");
  efiemu_exceptions_install();
  if (be32((CONST UINT8 *)(UINTN)fdt) != FDT_MAGIC) {
    serial_puts8((CONST CHAR8 *)"xnu-loader kernel: no device tree in x0\n");
    for (;;)
      __asm__ volatile("wfi");
  }
  parse_fdt(fdt);
  serial_puts8((CONST CHAR8 *)"xnu-loader kernel: device tree parsed, enabling the MMU\n");
  arm64_enable_mmu();
  serial_reinit();
  efiemu_debug_string("xnu-loader kernel: arm64 Image entry\n");
  log_ranges("ram", ram, nram);
  log_ranges("reserved", reserved, nreserved);

  UINT64 initrd = fdt_initrd_start;
  UINT64 initrd_size = fdt_initrd_end > fdt_initrd_start ? fdt_initrd_end - fdt_initrd_start : 0;
  if (embedded_initrd_end != embedded_initrd_start) {
    initrd = (UINT64)(UINTN)embedded_initrd_start;
    initrd_size = (UINT64)(embedded_initrd_end - embedded_initrd_start);
    efiemu_debug_string("xnu-loader kernel: using the built-in initrd\n");
  }
  if (initrd && initrd_size)
    kernel_parse_cpio(&boot_info, initrd, initrd_size);
  else
    efiemu_debug_string("xnu-loader kernel: no initrd\n");
  build_memory_map();
  if (cmdline[0])
    boot_info.cmdline = cmdline;
  if (boot_info.memory_count == 0) {
    efiemu_debug_string("xnu-loader kernel: device tree has no memory\n");
    for (;;)
      __asm__ volatile("wfi");
  }
  efiemu_main(&boot_info);
}
