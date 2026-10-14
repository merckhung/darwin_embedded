#include <stdint.h>
#include <stddef.h>

void boot32_jump(uint32_t entry, uint32_t boot_args) __attribute__((noreturn));

/* Console: QEMU virt's PL011 unless the FDT names a Rockchip board, whose console UART
 * is a DesignWare 8250 with 4-byte registers (THR +0x00, LSR +0x14) */
static volatile uint32_t *uart = (volatile uint32_t *)0x09000000;

// a board: console, watchdog, ram when u-boot's fdt has no memory node, identity
struct board {
  const char *fdt_compat, *name, *dt_compat, *model;
  uint32_t uart, wdt, cru_glb_rst, ram_size;
  // ram_base pairs with ram_size for that same fallback; sunxi puts dram at 0x40000000
  uint32_t ram_base;
  // the gic distributor, when the kernel may run smp from a secure boot
  uint32_t gicd;
  // cru gate and reset registers holding the watchdog's clocks, and its bits in both
  // (rockchip's designware watchdog; zero elsewhere, which also skips arming it)
  uint32_t wdt_gate, wdt_srst, wdt_bits;
};
static const struct board board_rv1106_info = {
  "rockchip,rv110", "Rockchip RV1106/RV1103", "puredarwin,rv1103", "Luckfox Pico (RV1103)",
  0xff4c0000, 0xff5a0000, 0xff3b0000 + 0xc10, 0x4000000, 0, 0, 0, 0, 0,
};
// wdt0, clocked and out of reset through CRU_GATE_CON06 and CRU_SOFTRST_CON06 bits 9 and 10
static const struct board board_rk3506_info = {
  "rockchip,rk3506", "Rockchip RK3506", "puredarwin,rk3506", "Luckfox Lyra (RK3506)",
  0xff0a0000, 0xff260000, 0xff9a0000 + 0xc10, 0x8000000, 0, 0xff581000,
  0xff9a0818, 0xff9a0a18, (1u << 9) | (1u << 10),
};

/* Allwinner A20 (sun7i): dual Cortex-A7, dram at 0x40000000, uart0 a dw-apb-uart with
 * 32-bit registers like rockchip's, gic-400 distributor at 0x01c81000. Its watchdog is
 * the sun4i one, not the designware block the fields above drive, so leave those zero. */
static const struct board board_a20_info = {
  "allwinner,sun7i-a20", "Allwinner A20 (sun7i)", "puredarwin,sun7i", "Allwinner A20",
  0x01c28000, 0, 0, 0x40000000, 0x40000000, 0x01c81000,
  0, 0, 0,
};

// cntvoff is unknown out of reset and only monitor mode with scr.ns set can write it; the
// kernel's cores all read the virtual counter, so zero it here and in the kernel's core pen
static void zero_cntvoff(void) {
  __asm__ volatile("cps #0x16\n\t"
                   "mrc p15, 0, r1, c1, c1, 0\n\t"
                   "orr r1, r1, #1\n\t"
                   "mcr p15, 0, r1, c1, c1, 0\n\t"
                   "isb\n\t"
                   "mov r0, #0\n\t"
                   "mov r2, #0\n\t"
                   "mcrr p15, 4, r0, r2, c14\n\t"
                   "bic r1, r1, #1\n\t"
                   "mcr p15, 0, r1, c1, c1, 0\n\t"
                   "isb\n\t"
                   "cps #0x13"
                   ::: "r0", "r1", "r2", "memory");
}

// a group register that takes a write means we run secure, as rockchip's usb boot leaves us
static int gic_secure(uint32_t gicd) {
  volatile uint32_t *igroupr0 = (volatile uint32_t *)(gicd + 0x80);
  uint32_t was = *igroupr0;
  int secure;

  *igroupr0 = ~0u;
  secure = *igroupr0 != 0;
  *igroupr0 = was;
  return secure;
}

// XNU_LOADER_PLATFORM_{RV1106,RK3506,QEMUVIRT} fix the board at build time
// without one the fdt's root compatible decides
#if (defined(XNU_LOADER_PLATFORM_RV1106) + defined(XNU_LOADER_PLATFORM_RK3506) + \
     defined(XNU_LOADER_PLATFORM_A20) + defined(XNU_LOADER_PLATFORM_QEMUVIRT)) > 1
#error "at most one XNU_LOADER_PLATFORM_* for arm32"
#endif
#if defined(XNU_LOADER_PLATFORM_RV1106)
static int uart_8250;
static const struct board *board = &board_rv1106_info;
#elif defined(XNU_LOADER_PLATFORM_RK3506)
static int uart_8250;
static const struct board *board = &board_rk3506_info;
#elif defined(XNU_LOADER_PLATFORM_A20)
static int uart_8250;
static const struct board *board = &board_a20_info;
// a fixed board talks on its own uart from the start, not virt's pl011 (unmapped there)
#define EARLY_UART 0x01c28000
// uart0 on pb22/pb23 at 115200 from the 24 MHz apb1, so output does not depend on u-boot
static void early_uart_setup(void) {
  volatile uint32_t *u = (volatile uint32_t *)EARLY_UART;
  volatile uint32_t *apb1_gate = (volatile uint32_t *)0x01c2006c;
  volatile uint32_t *pb_cfg2 = (volatile uint32_t *)0x01c2082c, *pb_pull1 = (volatile uint32_t *)0x01c20844;

  *apb1_gate |= 1u << 16;
  *pb_cfg2 = (*pb_cfg2 & ~(0x77u << 24)) | (0x22u << 24);
  *pb_pull1 = (*pb_pull1 & ~(3u << 14)) | (1u << 14);
  for (int i = 0; i < 100000 && !(u[5] & 0x40); i++)
    ;
  u[3] = 0x83;  /* LCR: 8n1, divisor latch */
  u[0] = 13;    /* 24 MHz / 16 / 115200 */
  u[1] = 0;
  u[3] = 0x03;
  u[2] = 0x07;  /* FCR: fifos on and cleared */
}
#else
static int uart_8250;
static const struct board *board;
#endif

static void putc(char c) {
  if (!uart)
    return;
  if (uart_8250) {
    while (!(uart[5] & 0x20))
      ;
  } else {
    while (uart[6] & 0x20)
      ;
  }
  uart[0] = (uint32_t)c;
}
static void puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      putc('\r');
    putc(*s++);
  }
}
static void puthex(uint32_t v) {
  puts("0x");
  for (int i = 7; i >= 0; --i)
    putc("0123456789abcdef"[(v >> (i * 4)) & 0xf]);
}
static void halt(const char *why) {
  puts("boot32: ");
  puts(why);
  puts("\n");
  for (;;)
    __asm__ volatile("wfi");
}

void *memset(void *d, int c, size_t n) {
  uint8_t *p = d;
  while (n--)
    *p++ = (uint8_t)c;
  return d;
}
/* Copies run upwards (the ramdisk slides down over itself), a word at a time when both
   sides allow it, and restart the board's watchdog so large images finish in time */
void *memcpy(void *d, const void *s, size_t n) {
  uint8_t *p = d;
  const uint8_t *q = s;
  if ((((uintptr_t)p | (uintptr_t)q) & 3) == 0) {
    while (n >= 4) {
      *(uint32_t *)p = *(const uint32_t *)q;
      p += 4;
      q += 4;
      n -= 4;
      if (board && board->wdt && ((uintptr_t)p & 0xfffff) == 0)
        ((volatile uint32_t *)board->wdt)[3] = 0x76;
    }
  }
  while (n--)
    *p++ = *q++;
  return d;
}
static size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}
static int strhas(const char *s, const char *w) {
  for (; *s; ++s) {
    const char *a = s, *b = w;
    while (*b && *a == *b) {
      ++a;
      ++b;
    }
    if (!*b)
      return 1;
  }
  return 0;
}
static int nameis(const char *name, const char *want) {
  while (*want && *name == *want) {
    ++name;
    ++want;
  }
  return !*want && (!*name || *name == '@');
}
// framebuffer, framebuffer@addr, or u-boot's sunxi framebuffer-lcd0-hdmi and kin
static int fbname(const char *name) {
  const char *w = "framebuffer";
  while (*w && *name == *w) {
    ++name;
    ++w;
  }
  return !*w && (!*name || *name == '@' || *name == '-');
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t cells(const uint8_t *p, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; ++i)
    v = v << 32 | be32(p + 4 * i);
  return v;
}
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Device tree facts */
static uint32_t ram_base, ram_size, initrd_start, initrd_end;
static char cmdline[256];
static const uint8_t *fdt_seed;
static uint32_t fdt_seed_len;
// the fdt's cpu nodes, by their reg (mpidr affinity), for xnu's /cpus
#define MAX_FDT_CPUS 4
static uint32_t cpu_reg[MAX_FDT_CPUS], ncpus;
// u-boot's live simple-framebuffer (sunxi fills one in /chosen with its hdmi mode)
static uint32_t fb_base, fb_width, fb_height, fb_stride, fb_bpp;

static void parse_fdt(const uint8_t *h) {
  const uint8_t *st = h + be32(h + 8);
  const char *strings = (const char *)(h + be32(h + 12));
  uint32_t ac = 2, sc = 1, depth = 0;
  enum { OTHER, MEMORY, CHOSEN, CPUS } kind = OTHER;
  int in_cpu = 0;
  // a framebuffer node at any depth: gather its props, keep it once it ends okay
  uint32_t fb_depth = 0, f_simple = 0, f_okay = 0, f_base = 0;
  uint32_t f_w = 0, f_h = 0, f_stride = 0, f_bpp = 0;
  // the #address-cells each node gives its children, which a framebuffer's reg is read with
  uint32_t acs[8] = { 2 };

  for (;;) {
    uint32_t tok = be32(st);
    st += 4;
    if (tok == 1) {
      const char *name = (const char *)st;
      st += (strlen(name) + 4) & ~3u;
      if (++depth < 8)
        acs[depth] = 2;
      if (depth == 2)
        kind = nameis(name, "memory") ? MEMORY : nameis(name, "chosen") ? CHOSEN :
               nameis(name, "cpus") ? CPUS : OTHER;
      else if (depth == 3 && kind == CPUS)
        in_cpu = nameis(name, "cpu");
      if (!fb_depth && fbname(name)) {
        fb_depth = depth;
        f_simple = f_base = f_w = f_h = f_stride = f_bpp = 0;
        f_okay = 1;
      }
    } else if (tok == 2) {
      if (fb_depth && depth == fb_depth) {
        fb_depth = 0;
        if (!fb_base && f_simple && f_okay && f_base && f_w && f_h && f_stride && f_bpp) {
          fb_base = f_base;
          fb_width = f_w;
          fb_height = f_h;
          fb_stride = f_stride;
          fb_bpp = f_bpp;
        }
      }
      if (depth == 3)
        in_cpu = 0;
      if (depth-- == 2)
        kind = OTHER;
    } else if (tok == 3) {
      uint32_t len = be32(st);
      const char *pn = strings + be32(st + 4);
      const uint8_t *v = st + 8;
      st += 8 + ((len + 3) & ~3u);
      if (fb_depth && depth == fb_depth) {
        if (streq(pn, "compatible"))
          f_simple = strhas((const char *)v, "simple-framebuffer");
        else if (streq(pn, "status"))
          f_okay = streq((const char *)v, "okay") || streq((const char *)v, "ok");
        else if (streq(pn, "reg") && fb_depth < 9 && len >= 4 * acs[fb_depth - 1])
          f_base = (uint32_t)cells(v, acs[fb_depth - 1]);
        else if (streq(pn, "width"))
          f_w = be32(v);
        else if (streq(pn, "height"))
          f_h = be32(v);
        else if (streq(pn, "stride"))
          f_stride = be32(v);
        else if (streq(pn, "format"))
          // the kernel draws 32-bit xrgb only
          f_bpp = streq((const char *)v, "x8r8g8b8") || streq((const char *)v, "a8r8g8b8") ? 32 : 0;
      } else if (depth == 1 && streq(pn, "compatible")) {
        for (uint32_t i = 0; i < len; i += strlen((const char *)v + i) + 1) {
#if !defined(XNU_LOADER_PLATFORM_QEMUVIRT)
          if (!board && strhas((const char *)v + i, board_rv1106_info.fdt_compat))
            board = &board_rv1106_info;
          else if (!board && strhas((const char *)v + i, board_rk3506_info.fdt_compat))
            board = &board_rk3506_info;
          else if (!board && strhas((const char *)v + i, board_a20_info.fdt_compat))
            board = &board_a20_info;
#endif
        }
      } else if (depth < 8 && streq(pn, "#address-cells")) {
        acs[depth] = be32(v);
        if (depth == 1)
          ac = acs[1];
      } else if (depth == 1 && streq(pn, "#size-cells")) {
        sc = be32(v);
      } else if (depth == 2 && kind == MEMORY && streq(pn, "reg") && !ram_size) {
        ram_base = (uint32_t)cells(v, ac);
        ram_size = (uint32_t)cells(v + 4 * ac, sc);
      } else if (depth == 3 && in_cpu && streq(pn, "reg") && len >= 4 && ncpus < MAX_FDT_CPUS) {
        cpu_reg[ncpus++] = be32(v + len - 4);
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "bootargs")) {
        uint32_t n = len < sizeof(cmdline) ? len : sizeof(cmdline) - 1;
        memcpy(cmdline, v, n);
        cmdline[n] = 0;
      } else if (depth == 2 && kind == CHOSEN &&
                 (streq(pn, "rng-seed") || (streq(pn, "kaslr-seed") && !fdt_seed))) {
        fdt_seed = v;
        fdt_seed_len = len;
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "linux,initrd-start")) {
        initrd_start = (uint32_t)cells(v, len / 4);
      } else if (depth == 2 && kind == CHOSEN && streq(pn, "linux,initrd-end")) {
        initrd_end = (uint32_t)cells(v, len / 4);
      }
    } else if (tok == 4) {
      continue;
    } else {
      break;
    }
  }
}

/* newc cpio: find a file by name */
static uint32_t hex8(const uint8_t *s) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    uint8_t c = s[i];
    v = v << 4 | (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                  c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
  }
  return v;
}
static const uint8_t *cpio_find(const char *want, uint32_t *size) {
  const uint8_t *base = (const uint8_t *)initrd_start;
  uint32_t off = 0, total = initrd_end - initrd_start;
  while (off + 110 <= total && (base[off] == '0')) {
    const uint8_t *h = base + off;
    uint32_t fsize = hex8(h + 54), nsize = hex8(h + 94);
    const char *name = (const char *)(h + 110);
    uint32_t data = (off + 110 + nsize + 3) & ~3u;
    if (streq(name, "TRAILER!!!"))
      break;
    if (streq(name, want)) {
      *size = fsize;
      return base + data;
    }
    off = (data + fsize + 3) & ~3u;
  }
  return 0;
}

/* Bytes of a newc cpio at p through its trailer, 0 if p holds none */
static uint32_t cpio_extent(const uint8_t *p, uint32_t limit) {
  uint32_t off = 0;
  if (p[0] != '0' || p[1] != '7' || p[2] != '0' || p[3] != '7' || p[4] != '0' || p[5] != '1')
    return 0;
  while (off + 110 <= limit && p[off] == '0') {
    const uint8_t *h = p + off;
    uint32_t fsize = hex8(h + 54), nsize = hex8(h + 94);
    uint32_t data = (off + 110 + nsize + 3) & ~3u;
    off = (data + fsize + 3) & ~3u;
    if (streq((const char *)(h + 110), "TRAILER!!!"))
      return off;
  }
  return 0;
}

/* Flattened xnu device tree: {nprops, nchildren}, props {name[32], len, value} */
struct dtbuf {
  uint8_t *p;
};
static uint32_t *dt_node(struct dtbuf *b, uint32_t nprops, uint32_t nchildren) {
  uint32_t *h = (uint32_t *)b->p;
  h[0] = nprops;
  h[1] = nchildren;
  b->p += 8;
  return h;
}
static void dt_prop(struct dtbuf *b, const char *name, const void *v, uint32_t len) {
  memset(b->p, 0, 32);
  memcpy(b->p, name, strlen(name));
  *(uint32_t *)(b->p + 32) = len;
  memcpy(b->p + 36, v, len);
  memset(b->p + 36 + len, 0, ((len + 3) & ~3u) - len);
  b->p += 36 + ((len + 3) & ~3u);
}
static void dt_str(struct dtbuf *b, const char *name, const char *s) {
  dt_prop(b, name, s, (uint32_t)strlen(s) + 1);
}
static void dt_u32(struct dtbuf *b, const char *name, uint32_t v) {
  dt_prop(b, name, &v, 4);
}

/* 64 seed bytes for xnu's PRNG: the FDT's rng-seed/kaslr-seed mixed with the counter */
static void make_seed(uint8_t *out) {
  uint32_t lo, hi;
  __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
  uint64_t x = ((uint64_t)hi << 32 | lo) ^ 0x9e3779b97f4a7c15ull;
  for (uint32_t i = 0; i < 64; ++i) {
    if (fdt_seed && i < fdt_seed_len)
      x ^= (uint64_t)fdt_seed[i] << (8 * (i & 7));
    x += 0x9e3779b97f4a7c15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    out[i] = (uint8_t)(z | 1);
  }
}

// paint band n (32 rows) of the boot framebuffer: progress that needs no uart
static void fb_band(uint32_t n, uint32_t xrgb) {
  if (!fb_base || fb_bpp != 32 || fb_height < 32 * (n + 1))
    return;
  for (uint32_t y = 32 * n; y < 32 * (n + 1); y++)
    for (uint32_t x = 0; x < fb_width; x++)
      ((volatile uint32_t *)(fb_base + y * fb_stride))[x] = xrgb;
}

struct boot_args32 {
  uint16_t Revision, Version;
  uint32_t virtBase, physBase, memSize, topOfKernelData;
  uint32_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
  uint32_t machineType;
  uint32_t deviceTreeP, deviceTreeLength;
  char CommandLine[256];
  uint32_t bootFlags, memSizeActual;
};

#define VIRT_BASE 0x80000000u
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

static uint32_t parse_hex(const char *s) {
  uint32_t v = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    s += 2;
  for (; *s; ++s) {
    char c = *s | 0x20;
    if (*s >= '0' && *s <= '9')
      v = v << 4 | (uint32_t)(*s - '0');
    else if (c >= 'a' && c <= 'f')
      v = v << 4 | (uint32_t)(c - 'a' + 10);
    else
      break; /* a stray character must not turn a size into garbage */
  }
  return v;
}

/* bootz: r2 = FDT. U-Boot 'go ADDR FDT [INITRD SIZE]': argc, argv, hex strings */
void boot32_main(uint32_t fdt, uint32_t argc, char **argv) {
  uint32_t go_initrd = 0, go_initrd_size = 0;
  // only a rockchip u-boot enters through 'go', the pico's unless the build names one
  const struct board *go_board = board ? board : &board_rv1106_info;
  uint32_t go_ram_top = go_board->ram_size;
  if (argc >= 1 && argc < 16 && argv != 0 && (uint32_t)argv < go_ram_top) {
    uint32_t i, nplain = 0;

    // talk on its uart before parsing anything
    board = go_board;
    uart = (volatile uint32_t *)board->uart;
    uart_8250 = 1;
    puts("\nboot32: entered via go\n");
    fdt = 0;
    for (i = 1; i < argc; i++) {
      uint32_t v = parse_hex(argv[i]);

      puts("boot32: argv["); puthex(i); puts("] = "); puts(argv[i]); puts("\n");
      if (fdt == 0 && v != 0 && v < go_ram_top && (v & 3) == 0 && be32((const uint8_t *)v) == 0xd00dfeed) {
        fdt = v;
      } else if (nplain == 0) {
        go_initrd = v;
        nplain++;
      } else if (nplain == 1) {
        go_initrd_size = v;
        nplain++;
      }
    }
    /* No FDT argument (an empty ${fdtcontroladdr}): U-Boot keeps its control FDT next to
     * its relocated self at the top of RAM, so take the first sane header up there */
    for (i = go_ram_top - 0x01000000; fdt == 0 && i < go_ram_top; i += 8) {
      const uint8_t *h = (const uint8_t *)i;

      if (be32(h) == 0xd00dfeed && be32(h + 4) >= 0x40 && be32(h + 4) < 0x100000 &&
          be32(h + 20) >= 16 && be32(h + 20) <= 17) {
        fdt = i;
        puts("boot32: found U-Boot's FDT at "); puthex(fdt); puts("\n");
      }
    }
  }
#ifdef EARLY_UART
  early_uart_setup();
  uart = (volatile uint32_t *)EARLY_UART;
  uart_8250 = 1;
  puts("\nboot32: entry, fdt ");
  puthex(fdt);
  puts("\n");
#endif
  if (be32((const uint8_t *)fdt) != 0xd00dfeed) {
    if (!uart_8250)
      uart[12] |= 0x301;
    halt("no device tree in r2");
  }
  parse_fdt((const uint8_t *)fdt);
  if (board) {
    uart = (volatile uint32_t *)board->uart; // u-boot left it configured
    uart_8250 = 1;
  } else if (!uart_8250) {
    uart[12] |= 0x301; /* PL011 CR: UARTEN, TXE, RXE (U-Boot leaves it on; QEMU does not) */
  }
  puts("\nboot32: xnu arm32 shim\n");
  if (board) {
    puts("boot32: board ");
    puts(board->name);
    puts("\n");
  }
  if (board && board->wdt && !strhas(cmdline, "pdnowdt")) {
    volatile uint32_t *wdt = (volatile uint32_t *)board->wdt;
    if (board->wdt_gate) {
      // rockchip hiword masks: clocks on, reset released
      *(volatile uint32_t *)board->wdt_gate = board->wdt_bits << 16;
      *(volatile uint32_t *)board->wdt_srst = board->wdt_bits << 16;
    }
    wdt[1] = 0xff;  /* TORR: TOP and TOP_INIT = 15 */
    wdt[3] = 0x76;  /* CRR: restart the count */
    wdt[0] = 0x1;   /* CR: enable, reset on expiry */
    /* CRU_GLB_RST_CON: let the watchdog trigger the first global reset, leaving the PMU alone */
    *(volatile uint32_t *)board->cru_glb_rst = (1u << 11) | (1u << 6) | (1u << 3);
    puts("boot32: watchdog armed\n");
  }
  if (board && board->gicd && gic_secure(board->gicd)) {
    zero_cntvoff();
    puts("boot32: secure, cntvoff zeroed\n");
  }
  if (go_initrd) {
    initrd_start = go_initrd;
    initrd_end = go_initrd + go_initrd_size;
  }
  /* A FIT or raw payload carries the cpio 64 KB past this image's start (bootm gives no initrd) */
  if (!initrd_start) {
    extern char _start[];
    const uint8_t *p = (const uint8_t *)_start + 0x10000;
    // it may run up to the end of ram when the fdt gave its size
    uint32_t lim = ram_size ? ram_base + ram_size - (uint32_t)p : 0x2000000;
    uint32_t n = cpio_extent(p, lim);
    if (n) {
      initrd_start = (uint32_t)p;
      initrd_end = initrd_start + n;
      puts("boot32: cpio after the image\n");
    }
  }
  if (!ram_size && board) {
    // u-boot's own fdt has no memory node, use the board's
    ram_base = board->ram_base;
    ram_size = board->ram_size;
  }
  // u-boot carves the framebuffer off the top of ram: end ram below it so the kernel leaves it be
  if (fb_base > ram_base && fb_base < ram_base + ram_size) {
    ram_size = (fb_base & ~0xfffu) - ram_base;
    puts("boot32: framebuffer ");
    puthex(fb_base);
    puts(", ");
    puthex(fb_width);
    puts(" x ");
    puthex(fb_height);
    puts("\n");
  }
  fb_band(0, 0xff2060ff);
  puts("boot32: RAM ");
  puthex(ram_base);
  puts(" + ");
  puthex(ram_size);
  puts(", initrd ");
  puthex(initrd_start);
  puts("\n");
  if (!initrd_start || initrd_end <= initrd_start)
    halt("no initrd: pass the kernel in a cpio with -initrd");

  uint32_t ksize = 0, asize = 0;
  const uint8_t *k = cpio_find("EFI/BOOT/kernel", &ksize);
  const uint8_t *a = cpio_find("EFI/BOOT/boot-args.txt", &asize);
  // keep the boot args before the ramdisk slides down over the cpio
  static char args_copy[256];
  if (a) {
    if (asize > sizeof(args_copy))
      asize = sizeof(args_copy);
    memcpy(args_copy, a, asize);
    a = (const uint8_t *)args_copy;
  }
  if (!k)
    halt("EFI/BOOT/kernel not in the initrd");
  if (rd32(k) != 0xfeedface || rd32(k + 4) != 12)
    halt("kernel is not a 32-bit ARM Mach-O");

  /* The kernel owns [physBase, RAM end): keep it clear of the shim and initrd. Its page
     tables need a 4 MB (ARM_TT_L1_PT_SIZE) boundary; 64 MB boards cannot spare 32 */
  uint32_t phys_base = ALIGN(initrd_end, board ? 0x400000u : 0x2000000u);
  uint32_t ncmds = rd32(k + 16), entry = 0, top = 0;
  const uint8_t *lc = k + 28;

  /* A 64 MB board cannot hold a ramdisk twice: when the payload was loaded high enough,
     put the kernel below it and slide the ramdisk down (copies run upwards) */
  uint32_t rsize = 0;
  const uint8_t *rd = cpio_find("EFI/BOOT/ramdisk.img", &rsize);
  if (board && rd) {
    uint32_t span = 0;
    for (uint32_t i = 0; i < ncmds; ++i, lc += rd32(lc + 4))
      if (rd32(lc) == 1 && rd32(lc + 28) && rd32(lc + 24) >= VIRT_BASE &&
          rd32(lc + 24) - VIRT_BASE + rd32(lc + 28) > span)
        span = rd32(lc + 24) - VIRT_BASE + rd32(lc + 28);
    lc = k + 28;
    uint32_t low = ALIGN(ram_base, 0x400000u) + 0x400000u;
    if (low + ALIGN(span, 0x1000) + 0x14000 <= initrd_start && (uint32_t)rd > (uint32_t)k)
      phys_base = low;
    puts("boot32: ramdisk in the cpio at ");
    puthex((uint32_t)rd);
    puts(", kernel span ");
    puthex(span);
    puts(", kernel at ");
    puthex(phys_base);
    puts("\n");
  }
  for (uint32_t i = 0; i < ncmds; ++i) {
    uint32_t cmd = rd32(lc), size = rd32(lc + 4);
    if (cmd == 1) { /* LC_SEGMENT */
      uint32_t vm = rd32(lc + 24), vms = rd32(lc + 28), fo = rd32(lc + 32), fs = rd32(lc + 36);
      if (vms && vm >= VIRT_BASE) {
        uint8_t *dst = (uint8_t *)(phys_base + vm - VIRT_BASE);
        memcpy(dst, k + fo, fs);
        memset(dst + fs, 0, vms - fs);
        if (vm - VIRT_BASE + vms > top)
          top = vm - VIRT_BASE + vms;
      }
    } else if (cmd == 5) { /* LC_UNIXTHREAD: r0-r12, sp, lr, pc */
      entry = rd32(lc + 16 + 15 * 4);
    }
    lc += size;
  }
  if (!entry)
    halt("kernel has no LC_UNIXTHREAD entry");

  uint32_t ba_phys = phys_base + ALIGN(top, 0x1000);
  struct boot_args32 *ba = (struct boot_args32 *)ba_phys;
  uint32_t dt_phys = ba_phys + 0x1000;
  struct dtbuf b = { (uint8_t *)dt_phys };

  /* Optional root ramdisk: xnu takes /chosen/memory-map/RAMDisk as md0, and maps it
     with ml_static_ptovirt, so it has to sit below topOfKernelData */
  uint32_t rd_phys = ALIGN(dt_phys + 0x10000, 0x4000);
  /* With the kernel below the payload, the ramdisk slides down only as far as the end of
     this shim's 64 KB (code, data and stack) */
  extern char _start[];
  if (rd && phys_base < (uint32_t)_start && rd_phys < (uint32_t)_start + 0x10000)
    rd_phys = ALIGN((uint32_t)_start + 0x10000, 0x4000);
  if (rd) {
    puts("boot32: moving the ramdisk\n");
    memcpy((void *)rd_phys, rd, rsize);
    rsize = ALIGN(rsize, 0x1000);
  }

  /* root: chosen (dram-base/size, memory-map), cpus (cpu0), defaults, options */
  dt_node(&b, 4, 4);
  dt_str(&b, "name", "device-tree");
  dt_str(&b, "compatible", board ? board->dt_compat : "puredarwin,virt-a7");
  dt_str(&b, "model", board ? board->model : "QEMU virt Cortex-A7");
  dt_u32(&b, "#address-cells", 1);
  dt_node(&b, 4, 1);
  dt_str(&b, "name", "chosen");
  dt_u32(&b, "dram-base", ram_base);
  dt_u32(&b, "dram-size", ram_size);
  uint8_t seed[64];
  make_seed(seed);
  dt_prop(&b, "random-seed", seed, sizeof(seed));
  uint32_t *mm = dt_node(&b, rd ? 2 : 1, 0);
  dt_str(&b, "name", "memory-map");
  if (rd) {
    uint32_t rdp[2] = { rd_phys, rsize };
    dt_prop(&b, "RAMDisk", rdp, sizeof(rdp));
  }
  // one node per fdt cpu, the boot cpu first as cpu0 and its reg the mpidr affinity
  if (ncpus == 0)
    cpu_reg[ncpus++] = 0;
  dt_node(&b, 1, ncpus);
  dt_str(&b, "name", "cpus");
  for (uint32_t i = 0; i < ncpus; i++) {
    char name[] = "cpu0";
    name[3] = (char)('0' + i);
    dt_node(&b, 4, 0);
    dt_str(&b, "name", name);
    dt_str(&b, "device_type", "cpu");
    dt_u32(&b, "reg", cpu_reg[i]);
    dt_str(&b, "state", i == 0 ? "running" : "waiting");
  }
  dt_node(&b, 1, 0);
  dt_str(&b, "name", "defaults");
  dt_node(&b, 1, 0);
  dt_str(&b, "name", "options");
  uint32_t dt_len = (uint32_t)(b.p - (uint8_t *)dt_phys);
  (void)mm;

  /* start.s builds its first translation table here, 16KB-aligned */
  uint32_t top_of_kernel = ALIGN(rd ? rd_phys + rsize : dt_phys + dt_len, 0x4000);
  if (dt_len > 0x10000)
    halt("device tree overlaps the ramdisk");

  memset(ba, 0, sizeof(*ba));
  ba->Revision = 2;
  ba->Version = 2;
  ba->virtBase = VIRT_BASE;
  ba->physBase = phys_base;
  ba->memSize = ram_base + ram_size - phys_base;
  ba->memSizeActual = ram_size;
  ba->topOfKernelData = top_of_kernel;
  ba->machineType = 0;
  if (fb_base) {
    // bit 0 marks a physical address; text mode so the verbose console draws on it
    ba->v_baseAddr = fb_base | 1;
    ba->v_display = 0;
    ba->v_rowBytes = fb_stride;
    ba->v_width = fb_width;
    ba->v_height = fb_height;
    ba->v_depth = fb_bpp;
  }
  ba->deviceTreeP = dt_phys - phys_base + VIRT_BASE;
  ba->deviceTreeLength = dt_len;
  if (a) {
    uint32_t n = asize < sizeof(ba->CommandLine) ? asize : sizeof(ba->CommandLine) - 1;
    memcpy(ba->CommandLine, a, n);
    for (uint32_t i = 0; i < n; ++i)
      if (ba->CommandLine[i] == '\n')
        ba->CommandLine[i] = ' ';
  } else {
    memcpy(ba->CommandLine, cmdline, sizeof(cmdline) - 1);
  }

  if (rd) {
    puts("boot32: ramdisk at ");
    puthex(rd_phys);
    puts(" + ");
    puthex(rsize);
    puts("\n");
  }
  puts("boot32: kernel at ");
  puthex(phys_base);
  puts(", entry ");
  puthex(entry);
  puts(", boot_args ");
  puthex(ba_phys);
  puts(", top ");
  puthex(top_of_kernel);
  puts("\n");
  fb_band(1, 0xff20c040);
#if defined(XNU_LOADER_PLATFORM_A20)
  *(volatile uint32_t *)0x01c2090c &= ~(1u << 24);  /* led off: at the kernel jump */
#endif
  boot32_jump(entry - VIRT_BASE + phys_base, ba_phys);
}
