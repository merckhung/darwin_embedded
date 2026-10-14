// Board description from the flattened device tree the firmware hands over: RAM base,
// the console UART and the GIC, so one arm64 build runs on any board with a sane DTB.
// Runs before the MMU is on in the Linux Image path, so every read is bytewise.
#include "fdt_board.h"
#include "platform.h"

#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)

#define FDT_MAGIC   0xd00dfeedU
#define TOK_BEGIN   1
#define TOK_END     2
#define TOK_PROP    3
#define TOK_NOP     4

#define MAX_DEPTH   16
#define PATH_LEN    256
#define MAX_ALIASES 16

FdtBoard g_board;

static UINT32 be32(CONST UINT8 *p) {
  return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static UINT64 cells(CONST UINT8 *p, UINT32 n) {
  UINT64 v = 0;
  for (UINT32 i = 0; i < n; ++i)
    v = v << 32 | be32(p + 4 * i);
  return v;
}

// no library calls: this runs before the MMU and caches are on
static VOID mem_zero(VOID *p, UINTN n) {
  volatile UINT8 *b = (volatile UINT8 *)p;
  for (UINTN i = 0; i < n; ++i)
    b[i] = 0;
}

static BOOLEAN mem_eq(CONST VOID *a, CONST VOID *b, UINTN n) {
  CONST UINT8 *x = (CONST UINT8 *)a, *y = (CONST UINT8 *)b;
  for (UINTN i = 0; i < n; ++i) {
    if (x[i] != y[i])
      return FALSE;
  }
  return TRUE;
}

static UINTN str_len(CONST CHAR8 *s) {
  UINTN n = 0;
  while (s[n])
    ++n;
  return n;
}

static BOOLEAN str_eq(CONST CHAR8 *a, CONST CHAR8 *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// a == b up to n bytes of a, with b ending there
static BOOLEAN str_eq_n(CONST CHAR8 *a, UINTN n, CONST CHAR8 *b) {
  for (UINTN i = 0; i < n; ++i) {
    if (a[i] != b[i])
      return FALSE;
  }
  return b[n] == 0;
}

static BOOLEAN str_prefix(CONST CHAR8 *s, CONST CHAR8 *prefix) {
  while (*prefix) {
    if (*s++ != *prefix++)
      return FALSE;
  }
  return TRUE;
}

// true when the nul-separated compatible list names want
static BOOLEAN compat_has(CONST CHAR8 *list, UINT32 len, CONST CHAR8 *want) {
  UINT32 i = 0;
  while (i < len) {
    if (str_eq(list + i, want))
      return TRUE;
    i += (UINT32)str_len(list + i) + 1;
  }
  return FALSE;
}

VOID fdt_board_defaults(VOID) {
  mem_zero(&g_board, sizeof(g_board));
  g_board.ram_base = 0x40000000ULL;
  g_board.uart_baud = 115200;
#if defined(XNU_LOADER_PLATFORM_QEMUVIRT)
  g_board.is_qemu_virt = TRUE;
  g_board.has_pci_ecam = TRUE;
  g_board.soc_base = 0x08000000ULL;
  g_board.soc_size = 0x08000000ULL;
  g_board.uart_kind = FDT_UART_PL011;
  g_board.uart_base = 0x09000000ULL;
  g_board.uart_size = 0x1000;
  g_board.uart_width = 4;
  g_board.gic_version = 3;
  g_board.gicd_base = 0x08000000ULL;
  g_board.gicd_size = 0x10000;
  g_board.gic2_base = 0x080a0000ULL;
  g_board.gic2_size = 0xf60000;
#elif defined(XNU_LOADER_PLATFORM_SUN50I)
  // allwinner h616/h618, orange pi zero 3
  g_board.soc_base = 0x01000000ULL;
  g_board.soc_size = 0x07000000ULL;
  g_board.uart_kind = FDT_UART_16550;
  g_board.uart_base = 0x05000000ULL;
  g_board.uart_size = 0x400;
  g_board.uart_shift = 2;
  g_board.uart_width = 4;
  g_board.uart_clock = 24000000;
  g_board.gic_version = 2;
  g_board.gicd_base = 0x03021000ULL;
  g_board.gicd_size = 0x1000;
  g_board.gic2_base = 0x03022000ULL;
  g_board.gic2_size = 0x2000;
#elif defined(XNU_LOADER_PLATFORM_SG2002)
  // sophgo sg2002 (licheerv nano) on its cortex-a53
  g_board.ram_base = 0x80000000ULL;
  g_board.soc_base = 0x01000000ULL;
  g_board.soc_size = 0x07000000ULL;
  g_board.uart_kind = FDT_UART_16550;
  g_board.uart_base = 0x04140000ULL;
  g_board.uart_size = 0x100;
  g_board.uart_shift = 2;
  g_board.uart_width = 4;
  g_board.uart_clock = 25000000;
  g_board.gic_version = 2;
  g_board.gicd_base = 0x01F01000ULL;
  g_board.gicd_size = 0x1000;
  g_board.gic2_base = 0x01F02000ULL;
  g_board.gic2_size = 0x2000;
#elif defined(XNU_LOADER_PLATFORM_SC8280XP)
  g_board.ram_base = 0x80000000ULL;
  g_board.soc_base = 0x17000000ULL;
  g_board.soc_size = 0x01000000ULL;
  g_board.psci_method = 2;
  g_board.gic_version = 3;
  g_board.gicd_base = 0x17a00000ULL;
  g_board.gicd_size = 0x10000;
  g_board.gic2_base = 0x17a60000ULL;
  g_board.gic2_size = 0x100000;
#endif
}

// console drivers by compatible, with the register layout a DTB without reg-shift implies
static CONST struct {
  CONST CHAR8 *compat;
  FdtUartKind kind;
  UINT32 shift, width;
} uart_table[] = {
  { "arm,pl011",                FDT_UART_PL011, 0, 4 },
  { "snps,dw-apb-uart",         FDT_UART_16550, 2, 4 },
  { "allwinner,uart-v100",      FDT_UART_16550, 2, 4 },
  { "allwinner,sun6i-a31-uart", FDT_UART_16550, 2, 4 },
  { "ns16550a",                 FDT_UART_16550, 0, 1 },
  { "ns16550",                  FDT_UART_16550, 0, 1 },
};

static CONST CHAR8 *gic3_compat[] = { "arm,gic-v3" };
static CONST CHAR8 *gic2_compat[] = { "arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a9-gic", "arm,gic-v2" };

typedef struct {
  UINT32 addr_cells, size_cells;   // for this node's children
  CONST UINT8 *ranges;
  UINT32 ranges_len;
  BOOLEAN has_ranges;
  UINTN path_len;
  CONST UINT8 *reg;
  UINT32 reg_len;
  CONST CHAR8 *compat;
  UINT32 compat_len;
  BOOLEAN disabled;
  BOOLEAN has_shift, has_width;
  UINT32 shift, width, clock, speed;
} Level;

typedef struct {
  CONST UINT8 *st;
  CONST CHAR8 *strings;
} Fdt;

// the console the firmware means: stdout-path, then console= on the command line, then serial0
typedef struct {
  CONST CHAR8 *stdout_path;
  UINT32 stdout_len;
  CONST CHAR8 *bootargs;
  UINT32 bootargs_len;
  CONST CHAR8 *alias_name[MAX_ALIASES];
  CONST CHAR8 *alias_path[MAX_ALIASES];
  UINT32 naliases;
  CONST UINT8 *mem_reg;
  UINT32 mem_len;
  UINT32 root_addr, root_size;
  UINT32 psci;
} Prescan;

static VOID prescan(Fdt *f, Prescan *p) {
  CONST UINT8 *st = f->st;
  UINT32 depth = 0;
  enum { N_OTHER, N_CHOSEN, N_ALIASES, N_MEMORY, N_PSCI } kind = N_OTHER;

  p->root_addr = 2;
  p->root_size = 1;
  for (;;) {
    UINT32 tok = be32(st);
    st += 4;
    if (tok == TOK_BEGIN) {
      CONST CHAR8 *name = (CONST CHAR8 *)st;
      st += (str_len(name) + 4) & ~3UL;
      ++depth;
      if (depth == 2) {
        kind = str_eq(name, "chosen") ? N_CHOSEN
               : str_eq(name, "aliases") ? N_ALIASES
               : (str_eq(name, "memory") || str_prefix(name, "memory@")) ? N_MEMORY
               : str_eq(name, "psci") ? N_PSCI
               : N_OTHER;
      }
    } else if (tok == TOK_END) {
      if (depth == 2)
        kind = N_OTHER;
      if (depth-- == 0)
        break;
    } else if (tok == TOK_PROP) {
      UINT32 len = be32(st);
      CONST CHAR8 *pname = f->strings + be32(st + 4);
      CONST UINT8 *v = st + 8;
      st += 8 + ((len + 3) & ~3U);
      if (depth == 1 && str_eq(pname, "#address-cells"))
        p->root_addr = be32(v);
      else if (depth == 1 && str_eq(pname, "#size-cells"))
        p->root_size = be32(v);
      else if (depth == 1 && str_eq(pname, "compatible"))
        g_board.is_qemu_virt = compat_has((CONST CHAR8 *)v, len, "linux,dummy-virt");
      else if (depth == 2 && kind == N_CHOSEN && str_eq(pname, "stdout-path")) {
        p->stdout_path = (CONST CHAR8 *)v;
        p->stdout_len = len;
      } else if (depth == 2 && kind == N_CHOSEN && str_eq(pname, "bootargs")) {
        p->bootargs = (CONST CHAR8 *)v;
        p->bootargs_len = len;
      } else if (depth == 2 && kind == N_ALIASES && p->naliases < MAX_ALIASES) {
        p->alias_name[p->naliases] = pname;
        p->alias_path[p->naliases++] = (CONST CHAR8 *)v;
      } else if (depth == 2 && kind == N_PSCI && str_eq(pname, "method")) {
        p->psci = str_eq((CONST CHAR8 *)v, "hvc") ? 1 : str_eq((CONST CHAR8 *)v, "smc") ? 2 : 0;
      } else if (depth == 2 && kind == N_MEMORY && str_eq(pname, "reg") && !p->mem_reg) {
        p->mem_reg = v;
        p->mem_len = len;
      }
    } else if (tok == TOK_NOP) {
      continue;
    } else {
      break;
    }
  }
}

static CONST CHAR8 *alias(Prescan *p, CONST CHAR8 *name, UINTN n) {
  for (UINT32 i = 0; i < p->naliases; ++i) {
    if (str_eq_n(name, n, p->alias_name[i]))
      return p->alias_path[i];
  }
  return NULL;
}

static UINT32 parse_uint(CONST CHAR8 *s, CONST CHAR8 *end) {
  UINT32 v = 0;
  while (s < end && *s >= '0' && *s <= '9')
    v = v * 10 + (UINT32)(*s++ - '0');
  return v;
}

// the path of the console node, and the baud when the firmware says one
static CONST CHAR8 *console_path(Prescan *p, UINT32 *baud) {
  if (p->stdout_path && p->stdout_len > 1) {
    CONST CHAR8 *s = p->stdout_path;
    UINTN n = 0;
    while (s[n] && s[n] != ':')
      ++n;
    if (s[n] == ':' && parse_uint(s + n + 1, s + p->stdout_len))
      *baud = parse_uint(s + n + 1, s + p->stdout_len);
    if (s[0] == '/')
      return s;   // compared up to the ':'
    return alias(p, s, n);
  }

  // console=ttyS<n>[,baud] or ttyAMA<n>, the last one wins as in Linux
  CONST CHAR8 *found = NULL;
  if (p->bootargs) {
    CONST CHAR8 *s = p->bootargs, *end = p->bootargs + p->bootargs_len;
    while (s < end && *s) {
      CONST CHAR8 *tty = NULL;
      if (str_prefix(s, "console=ttyS"))
        tty = s + 12;
      else if (str_prefix(s, "console=ttyAMA"))
        tty = s + 14;
      if (tty && *tty >= '0' && *tty <= '9') {
        CHAR8 name[16] = { 's', 'e', 'r', 'i', 'a', 'l' };
        UINTN n = 6;
        while (*tty >= '0' && *tty <= '9' && n < sizeof(name) - 1)
          name[n++] = *tty++;
        if (alias(p, name, n)) {
          found = alias(p, name, n);
          if (*tty == ',' && parse_uint(tty + 1, end))
            *baud = parse_uint(tty + 1, end);
        }
      }
      while (s < end && *s && *s != ' ')
        ++s;
      while (s < end && *s == ' ')
        ++s;
    }
  }
  if (found)
    return found;
  return alias(p, "serial0", 7);
}

// a node address in its parent's space, carried up through every ranges on the way to root
static BOOLEAN translate(Level *lv, UINT32 depth, UINT64 *addr) {
  for (UINT32 d = depth - 1; d >= 1; --d) {
    Level *bus = &lv[d];
    Level *parent = &lv[d - 1];
    if (!bus->has_ranges)
      return TRUE;
    if (bus->ranges_len == 0)
      continue;
    UINT32 child = bus->addr_cells, up = parent->addr_cells, size = bus->size_cells;
    UINT32 stride = 4 * (child + up + size);
    BOOLEAN hit = FALSE;
    for (UINT32 o = 0; o + stride <= bus->ranges_len; o += stride) {
      UINT64 cb = cells(bus->ranges + o, child);
      UINT64 pb = cells(bus->ranges + o + 4 * child, up);
      UINT64 sz = cells(bus->ranges + o + 4 * (child + up), size);
      if (*addr >= cb && *addr - cb < sz) {
        *addr = *addr - cb + pb;
        hit = TRUE;
        break;
      }
    }
    if (!hit)
      return FALSE;
  }
  return TRUE;
}

// the index-th (address, size) pair of a node's reg, in CPU physical addresses
static BOOLEAN node_reg(Level *lv, UINT32 depth, UINT32 index, UINT64 *base, UINT64 *size) {
  Level *node = &lv[depth], *parent = &lv[depth - 1];
  UINT32 ac = parent->addr_cells, sc = parent->size_cells;
  UINT32 off = index * 4 * (ac + sc);

  if (!node->reg || off + 4 * (ac + sc) > node->reg_len)
    return FALSE;
  *base = cells(node->reg + off, ac);
  *size = sc ? cells(node->reg + off + 4 * ac, sc) : 0;
  return translate(lv, depth, base);
}

static BOOLEAN path_is(CONST CHAR8 *path, UINTN n, CONST CHAR8 *want) {
  UINTN w = 0;
  while (want[w] && want[w] != ':')
    ++w;
  if (w != n)
    return FALSE;
  for (UINTN i = 0; i < n; ++i) {
    if (path[i] != want[i])
      return FALSE;
  }
  return TRUE;
}

static VOID node_done(Level *lv, UINT32 depth, CONST CHAR8 *path, CONST CHAR8 *console,
                      BOOLEAN *have_uart, BOOLEAN *have_gic) {
  Level *node = &lv[depth];
  UINT64 base, size;

  if (node->disabled || !node->compat)
    return;

  if (compat_has(node->compat, node->compat_len, "pci-host-ecam-generic"))
    g_board.has_pci_ecam = TRUE;

  if (!*have_uart && console && path_is(path, node->path_len, console)) {
    for (UINT32 i = 0; i < sizeof(uart_table) / sizeof(uart_table[0]); ++i) {
      if (!compat_has(node->compat, node->compat_len, uart_table[i].compat))
        continue;
      if (!node_reg(lv, depth, 0, &base, &size))
        break;
      g_board.uart_kind = uart_table[i].kind;
      g_board.uart_base = base;
      g_board.uart_size = size ? size : 0x1000;
      g_board.uart_shift = node->has_shift ? node->shift : uart_table[i].shift;
      g_board.uart_width = node->has_width ? node->width : uart_table[i].width;
      g_board.uart_clock = node->clock;
      if (node->speed)
        g_board.uart_baud = node->speed;
      *have_uart = TRUE;
      break;
    }
  }

  if (!*have_gic) {
    UINT32 version = 0;
    for (UINT32 i = 0; i < sizeof(gic3_compat) / sizeof(gic3_compat[0]); ++i)
      version = compat_has(node->compat, node->compat_len, gic3_compat[i]) ? 3 : version;
    for (UINT32 i = 0; i < sizeof(gic2_compat) / sizeof(gic2_compat[0]); ++i)
      version = compat_has(node->compat, node->compat_len, gic2_compat[i]) ? 2 : version;
    if (version && node_reg(lv, depth, 0, &g_board.gicd_base, &g_board.gicd_size) &&
        node_reg(lv, depth, 1, &g_board.gic2_base, &g_board.gic2_size)) {
      g_board.gic_version = version;
      *have_gic = TRUE;
    }
  }
}

BOOLEAN fdt_board_parse(CONST VOID *blob) {
  CONST UINT8 *h = (CONST UINT8 *)blob;
  Fdt f;
  Prescan p;
  Level lv[MAX_DEPTH];
  CHAR8 path[PATH_LEN];
  UINT32 baud = 0;
  INT32 depth = -1;
  BOOLEAN have_uart = FALSE, have_gic = FALSE;

  fdt_board_defaults();
  if (!h || be32(h) != FDT_MAGIC)
    return FALSE;

  f.st = h + be32(h + 8);
  f.strings = (CONST CHAR8 *)(h + be32(h + 12));
  mem_zero(&p, sizeof(p));
  prescan(&f, &p);

  // a board with a tree is described by it alone, not by the build's defaults
  BOOLEAN virt = g_board.is_qemu_virt;
  UINT64 soc_base = g_board.soc_base, soc_size = g_board.soc_size;
  mem_zero(&g_board, sizeof(g_board));
  g_board.from_fdt = TRUE;
  g_board.is_qemu_virt = virt;
  g_board.soc_base = soc_base;
  g_board.soc_size = soc_size;
  g_board.uart_baud = 115200;
  g_board.ram_base = 0x40000000ULL;
  g_board.psci_method = p.psci;

  if (p.mem_reg) {
    UINT32 stride = 4 * (p.root_addr + p.root_size);
    UINT64 lowest = ~0ULL;
    for (UINT32 o = 0; o + stride <= p.mem_len; o += stride) {
      UINT64 b = cells(p.mem_reg + o, p.root_addr);
      if (b < lowest)
        lowest = b;
    }
    if (lowest != ~0ULL)
      g_board.ram_base = lowest;
  }

  CONST CHAR8 *console = console_path(&p, &baud);
  if (baud)
    g_board.uart_baud = baud;

  CONST UINT8 *st = f.st;
  for (;;) {
    UINT32 tok = be32(st);
    st += 4;
    if (tok == TOK_BEGIN) {
      CONST CHAR8 *name = (CONST CHAR8 *)st;
      UINTN n = str_len(name);
      st += (n + 4) & ~3UL;
      if (++depth >= MAX_DEPTH)
        break;
      Level *l = &lv[depth];
      mem_zero(l, sizeof(*l));
      l->addr_cells = 2;
      l->size_cells = 1;
      if (depth == 0) {
        path[0] = '/';
        l->path_len = 1;
      } else {
        UINTN at = lv[depth - 1].path_len;
        if (at > 1 && at < PATH_LEN - 1)
          path[at++] = '/';
        for (UINTN i = 0; i < n && at < PATH_LEN - 1; ++i)
          path[at++] = name[i];
        l->path_len = at;
      }
    } else if (tok == TOK_END) {
      if (depth < 0)
        break;
      if (depth >= 1)
        node_done(lv, (UINT32)depth, path, console, &have_uart, &have_gic);
      if (--depth < 0)
        break;
    } else if (tok == TOK_PROP) {
      UINT32 len = be32(st);
      CONST CHAR8 *pname = f.strings + be32(st + 4);
      CONST UINT8 *v = st + 8;
      Level *l = &lv[depth];
      st += 8 + ((len + 3) & ~3U);
      if (depth < 0)
        break;
      if (str_eq(pname, "#address-cells"))
        l->addr_cells = be32(v);
      else if (str_eq(pname, "#size-cells"))
        l->size_cells = be32(v);
      else if (str_eq(pname, "ranges")) {
        l->has_ranges = TRUE;
        l->ranges = v;
        l->ranges_len = len;
      } else if (str_eq(pname, "reg")) {
        l->reg = v;
        l->reg_len = len;
      } else if (str_eq(pname, "compatible")) {
        l->compat = (CONST CHAR8 *)v;
        l->compat_len = len;
      } else if (str_eq(pname, "status")) {
        l->disabled = !str_eq((CONST CHAR8 *)v, "okay") && !str_eq((CONST CHAR8 *)v, "ok");
      } else if (str_eq(pname, "reg-shift") && len == 4) {
        l->has_shift = TRUE;
        l->shift = be32(v);
      } else if (str_eq(pname, "reg-io-width") && len == 4) {
        l->has_width = TRUE;
        l->width = be32(v);
      } else if (str_eq(pname, "clock-frequency") && len == 4) {
        l->clock = be32(v);
      } else if (str_eq(pname, "current-speed") && len == 4) {
        l->speed = be32(v);
      }
    } else if (tok == TOK_NOP) {
      continue;
    } else {
      break;
    }
  }

  // the arm-io window XNU offsets devices from: the span of what the kernel touches
  if (!g_board.soc_size) {
    UINT64 lo = ~0ULL, hi = 0;
    UINT64 b[3] = { g_board.uart_base, g_board.gicd_base, g_board.gic2_base };
    UINT64 s[3] = { g_board.uart_size, g_board.gicd_size, g_board.gic2_size };
    for (UINT32 i = 0; i < 3; ++i) {
      if (!b[i])
        continue;
      if (b[i] < lo)
        lo = b[i];
      if (b[i] + s[i] > hi)
        hi = b[i] + s[i];
    }
    if (hi) {
      g_board.soc_base = lo & ~0xFFFFFFULL;
      g_board.soc_size = (hi - g_board.soc_base + 0xFFFFFFULL) & ~0xFFFFFFULL;
    }
  }
  return TRUE;
}

CONST VOID *fdt_board_find(EFI_SYSTEM_TABLE *st) {
  static EFI_GUID dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

  if (!st)
    return NULL;
  for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &st->ConfigurationTable[i];
    if (mem_eq(&e->VendorGuid, &dtb_guid, sizeof(EFI_GUID)))
      return e->VendorTable;
  }
  return NULL;
}

#endif
