// the firmware fdt carried into the apple device tree the kernel reads, cells become native
// words and a 2-cell value one u64. riscv64 takes every node, arm64 the board's enabled devices
// next to the nodes the loader builds itself
#include "devtree.h"
#include "console.h"

#if defined(__riscv) || defined(__aarch64__)

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_MAX_DEPTH 16
#define MAX_IRQ_CONTROLLERS 256

static const EFI_GUID dtb_table_guid = { 0xb1b621d5, 0xf19c, 0x41a5,
                                         { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

typedef struct {
  DeviceTreeNode *node;
  // what this node says about its children, and what its parent says about it
  UINT32 addr_cells, size_cells;
  UINT32 parent_addr_cells, parent_size_cells;
  BOOLEAN has_addr_cells, has_size_cells, has_ranges;
  // interrupt wiring: the parent phandle is the node's own or the nearest ancestor's
  BOOLEAN own_irq_parent, has_irq_ext, is_irq_controller, added_irq_parent;
  UINT32 irq_parent;
  BOOLEAN skip;
  // status says the device is off, or the node is a pin controller whose children are pin groups
  BOOLEAN disabled, is_pinctrl;
  // the first address in reg, a cpu node's hart id
  BOOLEAN have_reg;
  UINT64 reg;
} FdtLevel;

// every node with both a phandle and #interrupt-cells, an interrupts-extended target
typedef struct {
  UINT32 phandle, cells;
} IrqController;

static IrqController irq_controllers[MAX_IRQ_CONTROLLERS];
static UINT32 irq_controller_count;

static UINT32 be32(const UINT8 *p) {
  return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static BOOLEAN str_eq(const CHAR8 *a, const CHAR8 *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

static BOOLEAN str_prefix(const CHAR8 *s, const CHAR8 *prefix) {
  while (*prefix && *s == *prefix) {
    ++s;
    ++prefix;
  }
  return *prefix == 0;
}

static BOOLEAN str_suffix(const CHAR8 *s, const CHAR8 *suffix) {
  UINTN n = 0, m = 0;
  while (s[n])
    ++n;
  while (suffix[m])
    ++m;
  return m <= n && str_eq(s + n - m, suffix);
}

static UINTN str_len(const CHAR8 *s) {
  UINTN n = 0;
  while (s[n])
    ++n;
  return n;
}

static const UINT8 *find_fdt(AppContext *ctx) {
  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    if (!CompareMem(&e->VendorGuid, &dtb_table_guid, sizeof(EFI_GUID))) {
      const UINT8 *h = (const UINT8 *)e->VendorTable;
      return h && be32(h) == FDT_MAGIC ? h : NULL;
    }
  }
  return NULL;
}

// dtc's guess: one or more non-empty printable strings, each nul terminated
static BOOLEAN looks_like_strings(const UINT8 *v, UINT32 len) {
  if (len == 0 || v[len - 1] != 0)
    return FALSE;
  UINT32 start = 0;
  for (UINT32 i = 0; i < len; i++) {
    if (v[i] == 0) {
      if (i == start)
        return FALSE;
      start = i + 1;
    } else if (v[i] < 0x20 || v[i] > 0x7e) {
      return FALSE;
    }
  }
  return TRUE;
}

// n big endian cells as the kernel reads them: 1 cell a u32, the last 2 cells one u64
static UINT32 put_value(UINT8 *out, const UINT8 *cells, UINT32 n) {
  UINT32 o = 0;
  for (UINT32 i = 0; n >= 2 && i < n - 2; i++, o += 4) {
    UINT32 c = be32(cells + 4 * i);
    CopyMem(out + o, &c, 4);
  }
  if (n == 1) {
    UINT32 c = be32(cells);
    CopyMem(out + o, &c, 4);
    o += 4;
  } else if (n >= 2) {
    UINT64 v = (UINT64)be32(cells + 4 * (n - 2)) << 32 | be32(cells + 4 * (n - 1));
    CopyMem(out + o, &v, 8);
    o += 8;
  }
  return o;
}

// repeated tuples of values, one count per field, as reg and ranges are laid out
static BOOLEAN put_tuples(UINT8 *out, const UINT8 *v, UINT32 len, const UINT32 *fields,
                          UINT32 nfields) {
  UINT32 stride = 0;
  for (UINT32 f = 0; f < nfields; f++)
    stride += fields[f];
  if (stride == 0 || len % (4 * stride) != 0)
    return FALSE;
  UINT32 o = 0;
  for (UINT32 at = 0; at < len;) {
    for (UINT32 f = 0; f < nfields; f++) {
      o += put_value(out + o, v + at, fields[f]);
      at += 4 * fields[f];
    }
  }
  return TRUE;
}

static void put_cells(UINT8 *out, const UINT8 *v, UINT32 len) {
  for (UINT32 i = 0; i + 4 <= len; i += 4) {
    UINT32 c = be32(v + i);
    CopyMem(out + i, &c, 4);
  }
}

static void add_prop(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name,
                     const VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *p = dt_create_property(ctx, name, (VOID *)data, len);
  if (p)
    dt_add_property(ctx, node, p);
}

static void add_u32(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, UINT32 v) {
  add_prop(ctx, node, name, &v, sizeof(v));
}

static void add_str(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, const CHAR8 *s) {
  add_prop(ctx, node, name, s, (UINT32)str_len(s) + 1);
}

// one fdt property onto an apple node, in the kernel's byte order
static void import_prop(AppContext *ctx, FdtLevel *lv, const CHAR8 *name, const UINT8 *v,
                        UINT32 len) {
  DeviceTreeNode *node = lv->node;
  UINT8 stack_buf[256];
  UINT8 *out = stack_buf;

  if (str_eq(name, "name"))
    return;
  if (len == 0) {
    add_prop(ctx, node, name, NULL, 0);
    return;
  }
  if (len > sizeof(stack_buf) &&
      EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, len,
                                  (VOID **)&out)))
    return;

  BOOLEAN numeric = str_eq(name, "reg") || str_eq(name, "ranges") ||
                    str_eq(name, "dma-ranges") || str_eq(name, "phandle") ||
                    str_eq(name, "linux,phandle") || str_eq(name, "interrupt-parent") ||
                    str_eq(name, "interrupts") || str_eq(name, "interrupts-extended") ||
                    name[0] == '#';
  BOOLEAN done = FALSE;

  if (!numeric && (looks_like_strings(v, len) || str_suffix(name, "-names"))) {
    CopyMem(out, v, len);
    done = TRUE;
  } else if (len % 4 != 0) {
    CopyMem(out, v, len);
    done = TRUE;
  } else if (str_eq(name, "reg")) {
    UINT32 f[2] = { lv->parent_addr_cells, lv->parent_size_cells };
    done = put_tuples(out, v, len, f, 2);
    if (done && lv->parent_addr_cells >= 1 && lv->parent_addr_cells <= 2) {
      lv->reg = lv->parent_addr_cells == 2 ? (UINT64)be32(v) << 32 | be32(v + 4) : be32(v);
      lv->have_reg = TRUE;
    }
  } else if (str_eq(name, "ranges") || str_eq(name, "dma-ranges")) {
    UINT32 f[3] = { lv->addr_cells, lv->parent_addr_cells, lv->size_cells };
    done = put_tuples(out, v, len, f, 3);
  } else if (len == 8 && (str_suffix(name, "-frequency") || str_prefix(name, "linux,initrd-"))) {
    // a frequency or an address that needed two cells is one native u64
    put_value(out, v, 2);
    done = TRUE;
  }
  if (!done)
    put_cells(out, v, len);

  add_prop(ctx, node, name, out, len);

  // SecureDTFindNodeWithPhandle looks for apple's spelling
  if (str_eq(name, "phandle"))
    add_prop(ctx, node, "AAPL,phandle", out, len);
  if (out != stack_buf)
    uefi_call_wrapper(ctx->bs->FreePool, 1, out);
}

// a node's properties all come before its children, so what the conversion needs is read ahead
static void scan_node(const UINT8 *st, const CHAR8 *strings, FdtLevel *lv, UINT32 *phandle,
                      UINT32 *icells) {
  for (;;) {
    UINT32 tok = be32(st);
    if (tok == FDT_NOP) {
      st += 4;
      continue;
    }
    if (tok != FDT_PROP)
      return;
    UINT32 len = be32(st + 4);
    const CHAR8 *pname = strings + be32(st + 8);
    const UINT8 *v = st + 12;
    if (len == 4 && str_eq(pname, "#address-cells")) {
      lv->addr_cells = be32(v);
      lv->has_addr_cells = TRUE;
    } else if (len == 4 && str_eq(pname, "#size-cells")) {
      lv->size_cells = be32(v);
      lv->has_size_cells = TRUE;
    } else if (str_eq(pname, "ranges")) {
      lv->has_ranges = TRUE;
    } else if (len >= 4 && str_eq(pname, "interrupt-parent")) {
      lv->irq_parent = be32(v);
      lv->own_irq_parent = TRUE;
    } else if (str_eq(pname, "interrupts-extended")) {
      lv->has_irq_ext = TRUE;
    } else if (str_eq(pname, "interrupt-controller")) {
      lv->is_irq_controller = TRUE;
    } else if (len == 4 && (str_eq(pname, "phandle") || str_eq(pname, "linux,phandle"))) {
      *phandle = be32(v);
    } else if (len == 4 && str_eq(pname, "#interrupt-cells")) {
      *icells = be32(v);
    } else if (str_eq(pname, "status") && looks_like_strings(v, len)) {
      lv->disabled = !str_eq((const CHAR8 *)v, "okay") && !str_eq((const CHAR8 *)v, "ok");
    } else if (str_eq(pname, "compatible") && looks_like_strings(v, len)) {
      for (UINT32 o = 0; o < len; o += (UINT32)str_len((const CHAR8 *)v + o) + 1)
        if (str_suffix((const CHAR8 *)v + o, "-pinctrl") || str_suffix((const CHAR8 *)v + o, "-pio"))
          lv->is_pinctrl = TRUE;
    }
    st += 12 + ((len + 3) & ~3U);
  }
}

static void collect_irq_controllers(const UINT8 *st, const CHAR8 *strings) {
  irq_controller_count = 0;
  for (;;) {
    UINT32 tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      st += (str_len((const CHAR8 *)st) + 4) & ~3UL;
      FdtLevel scratch;
      UINT32 phandle = 0, icells = ~0U;
      SetMem(&scratch, sizeof(scratch), 0);
      scan_node(st, strings, &scratch, &phandle, &icells);
      if (phandle && icells != ~0U && irq_controller_count < MAX_IRQ_CONTROLLERS) {
        irq_controllers[irq_controller_count].phandle = phandle;
        irq_controllers[irq_controller_count++].cells = icells;
      }
    } else if (tok == FDT_PROP) {
      st += 8 + ((be32(st) + 3) & ~3U);
    } else if (tok != FDT_END_NODE && tok != FDT_NOP) {
      return;
    }
  }
}

static BOOLEAN irq_controller_cells(UINT32 phandle, UINT32 *cells) {
  for (UINT32 i = 0; i < irq_controller_count; i++) {
    if (irq_controllers[i].phandle == phandle) {
      *cells = irq_controllers[i].cells;
      return TRUE;
    }
  }
  return FALSE;
}

// interrupts-extended on a device becomes interrupts plus one interrupt-parent per interrupt,
// the form IODTFindInterruptParent indexes, since iokit never reads interrupts-extended
static BOOLEAN split_interrupts_extended(AppContext *ctx, DeviceTreeNode *node, const UINT8 *v,
                                         UINT32 len) {
  UINT32 ncells = len / 4, nirq = 0, nspec = 0;
  UINT32 *parents = NULL, *specs = NULL;
  BOOLEAN ok = FALSE;

  if (len == 0 || len % 4 != 0 ||
      EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, 2 * len,
                                  (VOID **)&parents)))
    return FALSE;
  specs = parents + ncells;
  for (UINT32 at = 0; at < ncells;) {
    UINT32 phandle = be32(v + 4 * at), cells;
    if (!irq_controller_cells(phandle, &cells) || cells == 0 || at + 1 + cells > ncells)
      goto out;
    parents[nirq++] = phandle;
    for (UINT32 c = 0; c < cells; c++)
      specs[nspec++] = be32(v + 4 * (at + 1 + c));
    at += 1 + cells;
  }
  BOOLEAN one_parent = TRUE;
  for (UINT32 i = 1; i < nirq; i++)
    one_parent = one_parent && parents[i] == parents[0];
  add_prop(ctx, node, "interrupts", specs, 4 * nspec);
  add_prop(ctx, node, "interrupt-parent", parents, one_parent ? 4 : 4 * nirq);
  ok = TRUE;
out:
  uefi_call_wrapper(ctx->bs->FreePool, 1, parents);
  return ok;
}

static BOOLEAN node_is_cpu(DeviceTreeNode *node) {
  for (UINT32 i = 0; i < node->nProperties; i++) {
    DeviceTreeNodeProperty *p = node->properties[i];
    if (str_eq(p->name, "device_type") && p->value && p->length >= 4 &&
        str_eq((const CHAR8 *)p->value, "cpu"))
      return TRUE;
  }
  return FALSE;
}

#if defined(__aarch64__)
// nodes the arm64 loader builds itself or the kernel has no use for, and whatever is switched off.
// a vendor tree is hundreds of kilobytes, most of it disabled devices and pin groups
static BOOLEAN arm64_leave_out(UINT32 depth, const CHAR8 *name, FdtLevel *lv, FdtLevel *up) {
  static const CHAR8 *const own[] = { "cpus", "psci", "memory", "reserved-memory", "aliases",
                                      "__symbols__", "__fixups__", "__local_fixups__" };
  if (lv->disabled || up->is_pinctrl)
    return TRUE;
  if (depth != 1)
    return FALSE;
  for (UINT32 i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
    if (str_eq(name, own[i]))
      return TRUE;
  }
  return str_prefix(name, "memory@");
}
#endif

EFI_STATUS dt_import_fdt(AppContext *ctx, DeviceTreeNode *root, DeviceTreeNode *chosen) {
  const UINT8 *h = find_fdt(ctx);
  if (!h) {
    log_error(L"DT: no flattened device tree from firmware\r\n");
    return EFI_NOT_FOUND;
  }

  const UINT8 *st = h + be32(h + 8);
  const CHAR8 *strings = (const CHAR8 *)(h + be32(h + 12));
  FdtLevel levels[FDT_MAX_DEPTH];
  UINT32 depth = 0;
  UINT32 nodes = 0, cpus = 0;

  collect_irq_controllers(st, strings);

  for (;;) {
    UINT32 tok = be32(st);
    st += 4;
    if (tok == FDT_BEGIN_NODE) {
      const CHAR8 *name = (const CHAR8 *)st;
      UINTN len = str_len(name);
      st += (len + 4) & ~3UL;
      if (depth >= FDT_MAX_DEPTH) {
        log_error(L"DT: fdt nests deeper than %u\r\n", FDT_MAX_DEPTH);
        return EFI_UNSUPPORTED;
      }
      FdtLevel *lv = &levels[depth];
      FdtLevel *up = depth ? &levels[depth - 1] : NULL;
      SetMem(lv, sizeof(*lv), 0);
      // the devicetree spec defaults when a node leaves them out
      lv->addr_cells = 2;
      lv->size_cells = 1;
      UINT32 phandle = 0, icells = 0;
      scan_node(st, strings, lv, &phandle, &icells);
      lv->parent_addr_cells = up ? up->addr_cells : 2;
      lv->parent_size_cells = up ? up->size_cells : 1;
      if (!lv->own_irq_parent && up)
        lv->irq_parent = up->irq_parent;
      if (depth == 0) {
        lv->node = root;
      } else if (up->skip || (depth == 1 && str_eq(name, "chosen"))) {
        // /chosen is the loader's own, only stdout-path comes over
        lv->skip = TRUE;
#if defined(__aarch64__)
      } else if (arm64_leave_out(depth, name, lv, up)) {
        lv->skip = TRUE;
#endif
      } else {
        lv->node = dt_create_node(ctx);
        if (!lv->node)
          return EFI_OUT_OF_RESOURCES;
        add_str(ctx, lv->node, "name", name);
        dt_add_child(ctx, up->node, lv->node);
        nodes++;
      }
      // iokit's address resolution assumes 1 size cell where a bus says nothing
      if (lv->node && (depth == 0 || lv->has_ranges)) {
        if (!lv->has_addr_cells)
          add_u32(ctx, lv->node, "#address-cells", lv->addr_cells);
        if (!lv->has_size_cells)
          add_u32(ctx, lv->node, "#size-cells", lv->size_cells);
      }
      depth++;
    } else if (tok == FDT_END_NODE) {
      if (depth == 0)
        break;
      FdtLevel *lv = &levels[depth - 1];
#if defined(__riscv)
      // the kernel takes the cpu marked running as the boot hart
      if (lv->node && lv->node != root && lv->have_reg && node_is_cpu(lv->node)) {
        add_str(ctx, lv->node, "state", lv->reg == ctx->boot_hartid ? "running" : "waiting");
        cpus++;
      }
#endif
      depth--;
      if (depth == 0)
        break;
    } else if (tok == FDT_PROP) {
      UINT32 len = be32(st);
      const CHAR8 *pname = strings + be32(st + 4);
      const UINT8 *v = st + 8;
      st += 8 + ((len + 3) & ~3U);
      if (depth == 0)
        continue;
      FdtLevel *lv = &levels[depth - 1];
      if (lv->skip) {
        if (depth == 2 && chosen && str_eq(pname, "stdout-path") && looks_like_strings(v, len))
          add_prop(ctx, chosen, "stdout-path", v, len);
        continue;
      }
#if defined(__aarch64__)
      // the root keeps the loader's compatible and model, the platform expert matches on them
      if (depth == 1 && (str_eq(pname, "compatible") || str_eq(pname, "model")))
        continue;
#endif
      // a device's interrupts-extended outranks its interrupts and interrupt-parent
      BOOLEAN split = lv->has_irq_ext && !lv->is_irq_controller;
      if (split && str_eq(pname, "interrupts-extended") &&
          split_interrupts_extended(ctx, lv->node, v, len))
        continue;
      if (split && (str_eq(pname, "interrupts") || str_eq(pname, "interrupt-parent")))
        continue;
      import_prop(ctx, lv, pname, v, len);
      // an inherited interrupt-parent is written onto the device itself
      if (str_eq(pname, "interrupts") && !lv->own_irq_parent && lv->irq_parent &&
          !lv->added_irq_parent) {
        add_prop(ctx, lv->node, "interrupt-parent", &lv->irq_parent, sizeof(lv->irq_parent));
        lv->added_irq_parent = TRUE;
      }
    } else if (tok == FDT_NOP) {
      continue;
    } else {
      break;
    }
  }

#if defined(__riscv)
  log_info(L"DT: imported %u fdt nodes, %u cpus, boot hart %lu\r\n", nodes, cpus,
           ctx->boot_hartid);
#else
  (void)cpus;
  log_info(L"DT: imported %u fdt nodes\r\n", nodes);
#endif
  return EFI_SUCCESS;
}

BOOLEAN dt_riscv_fdt_is_qemu(AppContext *ctx) {
  const UINT8 *h = find_fdt(ctx);
  if (!h)
    return FALSE;
  const UINT8 *st = h + be32(h + 8);
  const CHAR8 *strings = (const CHAR8 *)(h + be32(h + 12));
  // the root's compatible is among its first properties
  if (be32(st) != FDT_BEGIN_NODE)
    return FALSE;
  st += 8;
  while (be32(st) == FDT_PROP || be32(st) == FDT_NOP) {
    if (be32(st) == FDT_NOP) {
      st += 4;
      continue;
    }
    UINT32 len = be32(st + 4);
    const CHAR8 *pname = strings + be32(st + 8);
    const CHAR8 *v = (const CHAR8 *)(st + 12);
    if (str_eq(pname, "compatible")) {
      for (UINT32 o = 0; o < len; o += (UINT32)str_len(v + o) + 1)
        if (str_prefix(v + o, "riscv-virtio") || str_prefix(v + o, "qemu,"))
          return TRUE;
      return FALSE;
    }
    st += 12 + ((len + 3) & ~3U);
  }
  return FALSE;
}

UINT32 dt_flat_size(DeviceTreeNode *node) {
  UINT32 size = 8;
  for (UINT32 i = 0; i < node->nProperties; i++)
    size += DT_MAX_NAME + 4 + ((node->properties[i]->length + 3) & ~3U);
  for (UINT32 i = 0; i < node->nChildren; i++)
    size += dt_flat_size(node->children[i]);
  return size;
}

#endif
