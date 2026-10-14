#include "devtree.h"
#include "console.h"
#include "app.h"
#include "boot.h"
#include "platform.h"
#include "fileio.h"
#include <efiprot.h>

/* One NVRAM bank. 0x2000 is the conventional CHRP size and leaves room for the
 * 0x800 common partition plus headers. */
#define XNU_NVRAM_BANK_SIZE 0x2000

/* /defaults serial-device points at /arm-io/uart0. Kept far above the phandles
 * dtc hands out from 1, since an imported board tree brings its own and the
 * kernel takes the first node that matches. */
#define XNU_LOADER_UART0_PHANDLE 0x7ff00001

#ifndef EFI_RNG_PROTOCOL_GUID
#define EFI_RNG_PROTOCOL_GUID \
  { 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 }}
typedef struct _EFI_RNG_PROTOCOL {
  EFI_STATUS (EFIAPI *GetInfo)(struct _EFI_RNG_PROTOCOL *This,
      UINTN *AlgorithmListSize, EFI_GUID *AlgorithmList);
  EFI_STATUS (EFIAPI *GetRNG)(struct _EFI_RNG_PROTOCOL *This,
      EFI_GUID *Algorithm, UINTN ValueLength, UINT8 *Value);
} EFI_RNG_PROTOCOL;
#endif

/*
 * Build one blank but *valid* CHRP NVRAM bank.
 *
 * It has to be valid, not merely zeroed. IONVRAMCHRPHandler::unserializeImage
 * bails at validateNVRAMVersion leaving _commonPartitionOffset = 0xFFFFFFFF
 * while _commonPartitionSize keeps its 0x800 constructor default, so
 * unserializeVariables then reads _nvramImage + 0xFFFFFFFF and the kernel takes
 * a data abort with far=0xffffffff. Real iBoot always hands over a valid image,
 * so XNU never guards that path.
 *
 * Layout: a 2-block "nvram" apple header partition, then a "common" partition
 * covering the rest of the bank with an empty (all-zero) variable area.
 */
#define CHRP_BLOCK 0x10

static UINT8 chrp_hdr_checksum(const UINT8 *hdr) {
  /* sum of sig + the len and name bytes, folded to 8 bits; cksum excluded */
  UINT16 sum = hdr[0];
  for (UINTN i = 2; i < CHRP_BLOCK; i++)
    sum += hdr[i];
  while (sum > 0xff)
    sum = (sum & 0xff) + (sum >> 8);
  return (UINT8)(sum & 0xff);
}

static UINT32 chrp_adler32(const UINT8 *buf, UINTN len) {
  UINT32 a = 1, b = 0;
  for (UINTN i = 0; i < len; i++) {
    a = (a + buf[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

static VOID chrp_build_blank_bank(UINT8 *bank, UINTN size) {
  const CHAR8 *hdr_name = (const CHAR8 *)"nvram";
  const CHAR8 *com_name = (const CHAR8 *)"common";
  UINTN i;

  SetMem(bank, size, 0);

  /* apple_nvram_header: chrp(16) + adler(4) + generation(4) + padding(8) */
  bank[0] = 0x5a;                       /* sig */
  bank[2] = 2; bank[3] = 0;             /* len, in 16-byte blocks */
  for (i = 0; hdr_name[i]; i++)
    bank[4 + i] = (UINT8)hdr_name[i];
  bank[1] = chrp_hdr_checksum(bank);
  /* generation at +20; adler at +16 is filled in last, over [+20, size) */
  bank[20] = 1;

  /* common partition fills the remainder */
  {
    UINT8 *com = bank + 2 * CHRP_BLOCK;
    UINT16 blocks = (UINT16)((size - 2 * CHRP_BLOCK) / CHRP_BLOCK);
    com[0] = 0x70;
    com[2] = (UINT8)(blocks & 0xff);
    com[3] = (UINT8)(blocks >> 8);
    for (i = 0; com_name[i]; i++)
      com[4 + i] = (UINT8)com_name[i];
    com[1] = chrp_hdr_checksum(com);
  }

  {
    UINT32 adler = chrp_adler32(bank + 20, size - 20);
    bank[16] = (UINT8)(adler & 0xff);
    bank[17] = (UINT8)((adler >> 8) & 0xff);
    bank[18] = (UINT8)((adler >> 16) & 0xff);
    bank[19] = (UINT8)((adler >> 24) & 0xff);
  }
}

static UINTN dt_align4(UINTN v) {
  return (v + 3) & ~((UINTN)3);
}

static UINTN dt_ascii_len(const CHAR8 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != '\0')
    n++;
  return n;
}

static UINTN dt_wcs_len(const CHAR16 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != 0)
    n++;
  return n;
}

DeviceTreeNode *dt_create_node(AppContext *ctx) {
  DeviceTreeNode *node = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode), (VOID **)&node);
  if (node)
    SetMem(node, sizeof(DeviceTreeNode), 0);
  return node;
}

DeviceTreeNodeProperty *dt_create_property(AppContext *ctx, const CHAR8 *name, VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *prop = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty), (VOID **)&prop);
  if (!prop)
    return NULL;

  SetMem(prop, sizeof(DeviceTreeNodeProperty), 0);

  UINTN n = dt_ascii_len(name);
  if (n >= DT_MAX_NAME)
    n = DT_MAX_NAME - 1;
  CopyMem(prop->name, name, n);
  prop->name[n] = 0;

  prop->length = len;
  if (len && data) {
    uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, len, (VOID **)&prop->value);
    if (prop->value)
      CopyMem(prop->value, data, len);
  }

  return prop;
}

VOID dt_add_property(AppContext *ctx, DeviceTreeNode *node, DeviceTreeNodeProperty *prop) {
  DeviceTreeNodeProperty **newProps = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNodeProperty*) * (node->nProperties + 1), (VOID **)&newProps);
  if (!newProps)
    return;

  if (node->properties) {
    CopyMem(newProps, node->properties, sizeof(DeviceTreeNodeProperty*) * node->nProperties);
    uefi_call_wrapper(ctx->bs->FreePool, 1, node->properties);
  }

  newProps[node->nProperties++] = prop;
  node->properties = newProps;
}

VOID dt_add_child(AppContext *ctx, DeviceTreeNode *parent, DeviceTreeNode *child) {
  DeviceTreeNode **newChildren = NULL;
  uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiBootServicesData, sizeof(DeviceTreeNode*) * (parent->nChildren + 1), (VOID **)&newChildren);
  if (!newChildren)
    return;

  if (parent->children) {
    CopyMem(newChildren, parent->children, sizeof(DeviceTreeNode*) * parent->nChildren);
    uefi_call_wrapper(ctx->bs->FreePool, 1, parent->children);
  }

  newChildren[parent->nChildren++] = child;
  parent->children = newChildren;
}

UINT32 dt_flatten_node(DeviceTreeNode *node, UINT8 *buf) {
  UINT32 offset = 0;

  CopyMem(buf + offset, &node->nProperties, sizeof(UINT32)); offset += sizeof(UINT32);
  CopyMem(buf + offset, &node->nChildren, sizeof(UINT32)); offset += sizeof(UINT32);

  for (UINT32 i = 0; i < node->nProperties; i++) {
    DeviceTreeNodeProperty *p = node->properties[i];
    UINTN padded = dt_align4(p->length);
    CopyMem(buf + offset, p->name, DT_MAX_NAME); offset += DT_MAX_NAME;
    CopyMem(buf + offset, &p->length, sizeof(UINT32)); offset += sizeof(UINT32);
    if (p->length && p->value)
      CopyMem(buf + offset, p->value, p->length);
    SetMem(buf + offset + p->length, padded - p->length, 0);
    offset += padded;
  }

  for (UINT32 i = 0; i < node->nChildren; i++) {
    offset += dt_flatten_node(node->children[i], buf + offset);
  }

  return offset;
}

static void dt_prop(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, VOID *data, UINT32 len) {
  DeviceTreeNodeProperty *p = dt_create_property(ctx, name, data, len);
  if (p)
    dt_add_property(ctx, node, p);
}

static void dt_prop_str(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, const CHAR8 *val) {
  UINT32 len = (UINT32)(dt_ascii_len(val) + 1);
  dt_prop(ctx, node, name, (VOID *)val, len);
}

static void dt_prop_u32(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, UINT32 val) {
  dt_prop(ctx, node, name, &val, 4);
}

/*
 * SMBIOS system UUID -> /options platform-uuid.
 *
 * XNU's IOPlatformExpertDevice::generatePlatformUUID() reads a 16-byte
 * "platform-uuid" OSData from /options on x86_64
 */
static BOOLEAN smbios_system_uuid(AppContext *ctx, UINT8 out[16]) {
  static const EFI_GUID smbios_guid  =
    {0xeb9d2d31,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
  static const EFI_GUID smbios3_guid =
    {0xf2fd1544,0x9794,0x4a2c,{0x99,0x2e,0xe5,0xbb,0xcf,0x20,0xe3,0x94}};

  UINT8 *tables = NULL;
  UINTN  tables_len = 0;

  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    UINT8 *ep = (UINT8 *)e->VendorTable;
    if (!ep) continue;

    if (!CompareMem(&e->VendorGuid, &smbios3_guid, sizeof(EFI_GUID))) {
      /* SMBIOS 3.x entry point: "_SM3_", 8-byte table address at 0x10. */
      if (ep[0]=='_' && ep[1]=='S' && ep[2]=='M' && ep[3]=='3' && ep[4]=='_') {
        UINT64 addr = 0;
        CopyMem(&addr, ep + 0x10, sizeof(addr));
        UINT32 maxlen = 0;
        CopyMem(&maxlen, ep + 0x0C, sizeof(maxlen));
        tables = (UINT8 *)(UINTN)addr;
        tables_len = maxlen;
        break;
      }
    } else if (!CompareMem(&e->VendorGuid, &smbios_guid, sizeof(EFI_GUID))) {
      /* SMBIOS 2.x entry point: "_SM_", 4-byte table address at 0x18. */
      if (ep[0]=='_' && ep[1]=='S' && ep[2]=='M' && ep[3]=='_') {
        UINT32 addr = 0;
        CopyMem(&addr, ep + 0x18, sizeof(addr));
        UINT16 len = 0;
        CopyMem(&len, ep + 0x16, sizeof(len));
        tables = (UINT8 *)(UINTN)addr;
        tables_len = len;
        /* keep looking: prefer a 3.x entry point if one also exists */
      }
    }
  }

  if (!tables || tables_len < 4) return FALSE;

  /* Walk the structure table for Type 1 (System Information). */
  UINT8 *p   = tables;
  UINT8 *end = tables + tables_len;
  while (p + 4 <= end) {
    UINT8 type = p[0];
    UINT8 hdr  = p[1];
    if (hdr < 4) break;
    if (p + hdr > end) break;

    if (type == 127) break;                  /* end-of-table */
    if (type == 1 && hdr >= 0x18) {          /* UUID lives at offset 0x08 */
      UINT8 *u = p + 0x08;
      /* Reject the all-zero / all-ones "not settable" encodings. */
      BOOLEAN all0 = TRUE, all1 = TRUE;
      for (UINTN i = 0; i < 16; i++) {
        if (u[i] != 0x00) all0 = FALSE;
        if (u[i] != 0xFF) all1 = FALSE;
      }
      if (!all0 && !all1) {
        /* SMBIOS >= 2.6 stores the first three fields little-endian; swap them
         * so the value matches what the host reports for this machine. */
        out[0]=u[3]; out[1]=u[2]; out[2]=u[1]; out[3]=u[0];
        out[4]=u[5]; out[5]=u[4];
        out[6]=u[7]; out[7]=u[6];
        for (UINTN i = 8; i < 16; i++) out[i] = u[i];
        return TRUE;
      }
    }

    /* Skip the formatted area, then the string set (double NUL terminated). */
    p += hdr;
    while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p++;
    p += 2;
  }
  return FALSE;
}

/* Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" into 16 raw bytes. */
/*
 * ACPI MADT ("APIC") lists one GICC entry per CPU (type 0x0B). Per the ACPI
 * spec the flags are at offset 12 (bit 0 = enabled) and the MPIDR at offset 68;
 * offsets 20 and 40 are the performance interrupt and the GICV base, which is
 * why reading those yielded zero for every CPU. Returns how many were stored.
 */
static UINTN acpi_cpu_mpidrs(AppContext *ctx, UINT64 *out, UINTN max) {
  static const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
  static const EFI_GUID acpi10 = ACPI_TABLE_GUID;
  UINT8 *rsdp = NULL;
  UINTN found = 0;

  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    if (!CompareMem(&e->VendorGuid, &acpi20, sizeof(EFI_GUID)) ||
        (rsdp == NULL && !CompareMem(&e->VendorGuid, &acpi10, sizeof(EFI_GUID))))
      rsdp = (UINT8 *)e->VendorTable;
  }
  if (!rsdp || CompareMem(rsdp, "RSD PTR ", 8))
    return 0;

  UINT8 *sdt = NULL;
  UINTN entry_size = 0;
  if (rsdp[15] >= 2) {                       /* revision 2+: XSDT, 8-byte entries */
    UINT64 xsdt = 0;
    CopyMem(&xsdt, rsdp + 24, sizeof(xsdt));
    sdt = (UINT8 *)(UINTN)xsdt;
    entry_size = 8;
  } else {
    UINT32 rsdt = 0;
    CopyMem(&rsdt, rsdp + 16, sizeof(rsdt));
    sdt = (UINT8 *)(UINTN)rsdt;
    entry_size = 4;
  }
  if (!sdt)
    return 0;

  UINT32 sdt_len = 0;
  CopyMem(&sdt_len, sdt + 4, sizeof(sdt_len));
  if (sdt_len < 36)
    return 0;

  for (UINTN off = 36; off + entry_size <= sdt_len; off += entry_size) {
    UINT64 addr = 0;
    CopyMem(&addr, sdt + off, entry_size);
    UINT8 *tbl = (UINT8 *)(UINTN)addr;
    if (!tbl || CompareMem(tbl, "APIC", 4))
      continue;

    UINT32 madt_len = 0;
    CopyMem(&madt_len, tbl + 4, sizeof(madt_len));
    for (UINTN e = 44; e + 2 <= madt_len && found < max;) {
      UINT8 type = tbl[e], len = tbl[e + 1];
      if (len < 2)
        break;
      if (type == 0x0B && len >= 76) {
        UINT32 flags = 0;
        UINT64 mpidr = 0;
        CopyMem(&flags, tbl + e + 12, sizeof(flags));
        CopyMem(&mpidr, tbl + e + 68, sizeof(mpidr));
        if (flags & 1)
          out[found++] = mpidr;
      }
      e += len;
    }
    break;
  }
  return found;
}

// booti hands over an fdt and no acpi, so the cpu list comes from its /cpus node
static UINT32 fdt_be32(const UINT8 *p) {
  return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static BOOLEAN fdt_str_eq(const CHAR8 *a, const CHAR8 *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

// each enabled cpu's MPIDR, and its capacity-dmips-mhz (0 when absent) for telling core types apart
static UINTN fdt_cpu_mpidrs(AppContext *ctx, UINT64 *out, UINT32 *cap, UINTN max) {
  static const EFI_GUID dtb_guid = { 0xb1b621d5, 0xf19c, 0x41a5,
                                     { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
  const UINT8 *h = NULL;
  UINTN found = 0;

  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    if (!CompareMem(&e->VendorGuid, &dtb_guid, sizeof(EFI_GUID)))
      h = (const UINT8 *)e->VendorTable;
  }
  if (!h || fdt_be32(h) != 0xd00dfeedU)
    return 0;

  const UINT8 *st = h + fdt_be32(h + 8);
  const CHAR8 *strings = (const CHAR8 *)(h + fdt_be32(h + 12));
  UINT32 depth = 0, cells = 1;
  BOOLEAN in_cpus = FALSE, is_cpu = FALSE, disabled = FALSE, have_reg = FALSE;
  UINT64 reg = 0;
  UINT32 capacity = 0;

  for (;;) {
    UINT32 tok = fdt_be32(st);
    st += 4;
    if (tok == 1) {                                    // begin node
      const CHAR8 *name = (const CHAR8 *)st;
      UINTN len = 0;
      while (name[len])
        ++len;
      st += (len + 4) & ~3UL;
      ++depth;
      if (depth == 2)
        in_cpus = fdt_str_eq(name, "cpus");
      if (depth == 3 && in_cpus) {
        is_cpu = disabled = have_reg = FALSE;
        reg = 0;
        capacity = 0;
      }
    } else if (tok == 2) {                             // end node
      if (depth == 3 && in_cpus && is_cpu && have_reg && !disabled && found < max) {
        cap[found] = capacity;
        out[found++] = reg;
      }
      if (depth == 2)
        in_cpus = FALSE;
      --depth;
    } else if (tok == 3) {                             // property
      UINT32 len = fdt_be32(st);
      const CHAR8 *pname = strings + fdt_be32(st + 4);
      const UINT8 *v = st + 8;
      st += 8 + ((len + 3) & ~3U);
      if (depth == 2 && in_cpus && fdt_str_eq(pname, "#address-cells") && len >= 4) {
        cells = fdt_be32(v);
      } else if (depth == 3 && in_cpus) {
        if (fdt_str_eq(pname, "device_type"))
          is_cpu = fdt_str_eq((const CHAR8 *)v, "cpu");
        else if (fdt_str_eq(pname, "status"))
          disabled = !fdt_str_eq((const CHAR8 *)v, "okay") && !fdt_str_eq((const CHAR8 *)v, "ok");
        else if (fdt_str_eq(pname, "reg") && len >= 4 * cells) {
          reg = 0;
          for (UINT32 c = 0; c < cells; c++)
            reg = reg << 32 | fdt_be32(v + 4 * c);
          have_reg = TRUE;
        } else if (fdt_str_eq(pname, "capacity-dmips-mhz") && len >= 4) {
          capacity = fdt_be32(v);
        }
      }
    } else if (tok == 4) {                             // nop
      continue;
    } else {
      break;
    }
  }
  return found;
}

static BOOLEAN uuid_str_to_bytes(const CHAR8 *str, UINT8 out[16]) {
  UINTN oi = 0;
  for (UINTN i = 0; str[i] && oi < 16; i++) {
    if (str[i] == '-') continue;
    CHAR8 hi = str[i], lo = str[i + 1];
    if (!lo) return FALSE;
    UINT8 v = 0;
    for (UINTN half = 0; half < 2; half++) {
      CHAR8 c = half ? lo : hi;
      UINT8 n;
      if (c >= '0' && c <= '9') n = (UINT8)(c - '0');
      else if (c >= 'a' && c <= 'f') n = (UINT8)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') n = (UINT8)(c - 'A' + 10);
      else return FALSE;
      v = (UINT8)((v << 4) | n);
    }
    out[oi++] = v;
    i++;
  }
  return oi == 16;
}


static void dt_prop_u64(AppContext *ctx, DeviceTreeNode *node, const CHAR8 *name, UINT64 val) {
  dt_prop(ctx, node, name, &val, 8);
}

#if defined(PD_ARCH_X86)
static UINT64 rdtsc64_raw(void) {
  UINT32 lo, hi;
  __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((UINT64)hi << 32) | lo;
}

static UINT64 penryn_bus_frequency(UINT64 tsc_frequency) {
  UINT32 eax, ebx, ecx, edx;
  __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                    : "a"(0), "c"(0));
  if (ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e)
    return 0;
  __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                    : "a"(1), "c"(0));
  /* Match XNU's CPUFAMILY_INTEL_PENRYN selection: family 6, model 23.
   * Only this model uses IA32_PERF_STATUS bits 44:40 and the half ratio. */
  if (((eax >> 8) & 15) != 6 ||
      (((eax >> 4) & 15) | ((eax >> 12) & 0xf0)) != 23)
    return 0;
  UINT32 lo, hi;
  __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x198));
  UINT32 twice_ratio = 2 * ((hi >> 8) & 31) + ((hi >> 14) & 1);
  if (!twice_ratio || !tsc_frequency)
    return 0;
  return 2 * tsc_frequency / twice_ratio;
}
#elif defined(__aarch64__)
/* Real, not a stub: CNTVCT_EL0 is arm64's equivalent free-running counter
 * (its rate comes from CNTFRQ_EL0, unlike TSC's calibration dance below,
 * but the raw-read shape matches what callers here want). */
static UINT64 rdtsc64_raw(void) {
  UINT64 v;
  __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
#elif defined(__riscv)
// the time csr, ticking at /cpus timebase-frequency
static UINT64 rdtsc64_raw(void) {
  UINT64 v;
  __asm__ volatile ("rdtime %0" : "=r"(v));
  return v;
}
#else
#error "devtree.c: unsupported architecture"
#endif

static UINT64 estimate_tsc_frequency(AppContext *ctx, UINT64 *initial_tsc) {
  const UINTN usec = 20000;
  UINT64 start = rdtsc64_raw();
  uefi_call_wrapper(ctx->bs->Stall, 1, usec);
  UINT64 end = rdtsc64_raw();

  if (initial_tsc)
    *initial_tsc = start;
  if (end <= start)
    return 0;

  return ((end - start) * 1000000ULL) / usec;
}

/*
 * Fixed physical page for the XNU flat device tree.
 *
 * Pinned at a known physical address (below 0xF0000) using AllocateMaxAddress.
 * Must survive ExitBootServices and not be in OVMF's DEBUG-fill zone.
 */

/* Format a 16-byte EFI GUID (LE Data1/2/3, BE Data4) as a UUID string.
 * out37 must be at least 37 bytes. */
static void fmt_guid_uuid(const UINT8 *g, CHAR8 *out37) {
  static const CHAR8 H[] = "0123456789ABCDEF";
  UINTN i = 0;
  /* Data1: 4 LE bytes, printed big-endian */
  for (int b = 3; b >= 0; b--) { out37[i++]=H[g[b]>>4]; out37[i++]=H[g[b]&0xF]; }
  out37[i++] = '-';
  /* Data2: 2 LE bytes */
  out37[i++]=H[g[5]>>4]; out37[i++]=H[g[5]&0xF];
  out37[i++]=H[g[4]>>4]; out37[i++]=H[g[4]&0xF];
  out37[i++] = '-';
  /* Data3: 2 LE bytes */
  out37[i++]=H[g[7]>>4]; out37[i++]=H[g[7]&0xF];
  out37[i++]=H[g[6]>>4]; out37[i++]=H[g[6]&0xF];
  out37[i++] = '-';
  /* Data4[0..1] */
  out37[i++]=H[g[8]>>4]; out37[i++]=H[g[8]&0xF];
  out37[i++]=H[g[9]>>4]; out37[i++]=H[g[9]&0xF];
  out37[i++] = '-';
  /* Data4[2..7] */
  for (int b = 10; b < 16; b++) { out37[i++]=H[g[b]>>4]; out37[i++]=H[g[b]&0xF]; }
  out37[i] = '\0';
}

static BOOLEAN is_uuid_char(CHAR8 c) {
  return (c >= '0' && c <= '9') ||
         (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F') ||
         c == '-';
}

static BOOLEAN boot_arg_get_uuid(const CHAR8 *boot_args, const CHAR8 *key, CHAR8 uuid_str[37]) {
  UINTN key_len = 0;
  while (key[key_len]) key_len++;

  for (const CHAR8 *p = boot_args; p && *p; p++) {
    if (p != boot_args && p[-1] != ' ')
      continue;

    UINTN i = 0;
    while (i < key_len && p[i] == key[i]) i++;
    if (i != key_len || p[i] != '=')
      continue;

    p += key_len + 1;
    for (i = 0; i < 36; i++) {
      if (!is_uuid_char(p[i]))
        return FALSE;
      uuid_str[i] = p[i];
    }
    if (p[36] && p[36] != ' ')
      return FALSE;
    uuid_str[36] = '\0';
    return TRUE;
  }

  return FALSE;
}

static BOOLEAN boot_arg_get_value(const CHAR8 *boot_args, const CHAR8 *key,
                                  CHAR8 *out, UINTN out_size) {
  if (!boot_args || !key || !out || out_size == 0)
    return FALSE;

  UINTN key_len = 0;
  while (key[key_len]) key_len++;

  for (const CHAR8 *p = boot_args; *p; p++) {
    if (p != boot_args && p[-1] != ' ')
      continue;

    UINTN i = 0;
    while (i < key_len && p[i] == key[i]) i++;
    if (i != key_len || p[i] != '=')
      continue;

    p += key_len + 1;
    UINTN n = 0;
    while (p[n] && p[n] != ' ' && n + 1 < out_size) {
      out[n] = p[n];
      n++;
    }
    out[n] = '\0';
    return n != 0;
  }

  return FALSE;
}

typedef struct {
  UINT32 state[4];
  UINT64 bit_count;
  UINT8 buffer[64];
} Md5Context;

static UINT32 md5_rol(UINT32 x, UINT32 n) {
  return (x << n) | (x >> (32 - n));
}

static UINT32 rd32le(const UINT8 *p) {
  return (UINT32)p[0] | ((UINT32)p[1] << 8) |
         ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 rd64le(const UINT8 *p) {
  return (UINT64)rd32le(p) | ((UINT64)rd32le(p + 4) << 32);
}

static UINT16 rd16be(const UINT8 *p) {
  return ((UINT16)p[0] << 8) | (UINT16)p[1];
}

static void wr32le(UINT8 *p, UINT32 v) {
  p[0] = (UINT8)v;
  p[1] = (UINT8)(v >> 8);
  p[2] = (UINT8)(v >> 16);
  p[3] = (UINT8)(v >> 24);
}

static void md5_transform(UINT32 state[4], const UINT8 block[64]) {
  static const UINT32 k[64] = {
    0xd76aa478U,0xe8c7b756U,0x242070dbU,0xc1bdceeeU,
    0xf57c0fafU,0x4787c62aU,0xa8304613U,0xfd469501U,
    0x698098d8U,0x8b44f7afU,0xffff5bb1U,0x895cd7beU,
    0x6b901122U,0xfd987193U,0xa679438eU,0x49b40821U,
    0xf61e2562U,0xc040b340U,0x265e5a51U,0xe9b6c7aaU,
    0xd62f105dU,0x02441453U,0xd8a1e681U,0xe7d3fbc8U,
    0x21e1cde6U,0xc33707d6U,0xf4d50d87U,0x455a14edU,
    0xa9e3e905U,0xfcefa3f8U,0x676f02d9U,0x8d2a4c8aU,
    0xfffa3942U,0x8771f681U,0x6d9d6122U,0xfde5380cU,
    0xa4beea44U,0x4bdecfa9U,0xf6bb4b60U,0xbebfbc70U,
    0x289b7ec6U,0xeaa127faU,0xd4ef3085U,0x04881d05U,
    0xd9d4d039U,0xe6db99e5U,0x1fa27cf8U,0xc4ac5665U,
    0xf4292244U,0x432aff97U,0xab9423a7U,0xfc93a039U,
    0x655b59c3U,0x8f0ccc92U,0xffeff47dU,0x85845dd1U,
    0x6fa87e4fU,0xfe2ce6e0U,0xa3014314U,0x4e0811a1U,
    0xf7537e82U,0xbd3af235U,0x2ad7d2bbU,0xeb86d391U
  };
  static const UINT8 s[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
  };
  UINT32 m[16];
  UINT32 a = state[0], b = state[1], c = state[2], d = state[3];

  for (UINTN i = 0; i < 16; i++)
    m[i] = rd32le(block + i * 4);

  for (UINTN i = 0; i < 64; i++) {
    UINT32 f, g;
    if (i < 16) {
      f = (b & c) | ((~b) & d);
      g = (UINT32)i;
    } else if (i < 32) {
      f = (d & b) | ((~d) & c);
      g = (UINT32)((5 * i + 1) & 15);
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (UINT32)((3 * i + 5) & 15);
    } else {
      f = c ^ (b | (~d));
      g = (UINT32)((7 * i) & 15);
    }
    UINT32 tmp = d;
    d = c;
    c = b;
    b = b + md5_rol(a + f + k[i] + m[g], s[i]);
    a = tmp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static void md5_init(Md5Context *ctx) {
  ctx->bit_count = 0;
  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xefcdab89U;
  ctx->state[2] = 0x98badcfeU;
  ctx->state[3] = 0x10325476U;
  SetMem(ctx->buffer, sizeof(ctx->buffer), 0);
}

static void md5_update(Md5Context *ctx, const UINT8 *data, UINTN len) {
  UINTN index = (UINTN)((ctx->bit_count >> 3) & 63);
  ctx->bit_count += (UINT64)len << 3;

  UINTN part_len = 64 - index;
  UINTN i = 0;
  if (len >= part_len) {
    CopyMem(&ctx->buffer[index], data, part_len);
    md5_transform(ctx->state, ctx->buffer);
    for (i = part_len; i + 63 < len; i += 64)
      md5_transform(ctx->state, data + i);
    index = 0;
  }
  if (i < len)
    CopyMem(&ctx->buffer[index], data + i, len - i);
}

static void md5_final(Md5Context *ctx, UINT8 digest[16]) {
  UINT8 bits[8];
  UINT8 pad[64];
  UINTN index = (UINTN)((ctx->bit_count >> 3) & 63);
  UINTN pad_len = (index < 56) ? (56 - index) : (120 - index);

  for (UINTN i = 0; i < 8; i++)
    bits[i] = (UINT8)(ctx->bit_count >> (8 * i));
  SetMem(pad, sizeof(pad), 0);
  pad[0] = 0x80;

  md5_update(ctx, pad, pad_len);
  md5_update(ctx, bits, sizeof(bits));

  for (UINTN i = 0; i < 4; i++)
    wr32le(digest + i * 4, ctx->state[i]);
}

static void fmt_uuid_bytes(const UINT8 uuid[16], CHAR8 *out37) {
  static const CHAR8 H[] = "0123456789ABCDEF";
  UINTN j = 0;
  for (UINTN i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out37[j++] = '-';
    out37[j++] = H[uuid[i] >> 4];
    out37[j++] = H[uuid[i] & 0xF];
  }
  out37[j] = '\0';
}

static void hfs_uuid_from_finder_info(const UINT8 hfs_uuid[8], CHAR8 uuid_str[37]) {
  static const UINT8 fs_uuid_namespace[16] = {
    0xB3,0xE2,0x0F,0x39,0xF2,0x92,0x11,0xD6,
    0x97,0xA4,0x00,0x30,0x65,0x43,0xEC,0xAC
  };
  UINT8 uuid[16];
  Md5Context md5;

  if ((hfs_uuid[0] == 0 && hfs_uuid[1] == 0 && hfs_uuid[2] == 0 && hfs_uuid[3] == 0) ||
      (hfs_uuid[4] == 0 && hfs_uuid[5] == 0 && hfs_uuid[6] == 0 && hfs_uuid[7] == 0)) {
    SetMem(uuid, sizeof(uuid), 0);
  } else {
    md5_init(&md5);
    md5_update(&md5, fs_uuid_namespace, sizeof(fs_uuid_namespace));
    md5_update(&md5, hfs_uuid, 8);
    md5_final(&md5, uuid);
    uuid[6] = (uuid[6] & 0x0F) | 0x30;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
  }

  fmt_uuid_bytes(uuid, uuid_str);
}

typedef struct {
  UINT64 Revision;
  EFI_BLOCK_IO_MEDIA *MediaInfo;
  EFI_STATUS (EFIAPI *Reset)(VOID *This, BOOLEAN ExtendedVerification);
  EFI_STATUS (EFIAPI *ReadBlocks)(VOID *This, UINT32 MediaId, EFI_LBA LBA,
                                  UINTN BufferSize, VOID *Buffer);
  EFI_STATUS (EFIAPI *WriteBlocks)(VOID *This, UINT32 MediaId, EFI_LBA LBA,
                                   UINTN BufferSize, VOID *Buffer);
  EFI_STATUS (EFIAPI *FlushBlocks)(VOID *This);
} XnuBlockIoProtocol;

static BOOLEAN guid_eq_raw(const UINT8 *a, const UINT8 *b) {
  for (UINTN i = 0; i < 16; i++) {
    if (a[i] != b[i])
      return FALSE;
  }
  return TRUE;
}

static BOOLEAN read_hfs_uuid_from_blockio(XnuBlockIoProtocol *bio, CHAR8 uuid_str[37]) {
  static const UINT8 apple_hfs_guid[16] = {
    0x00,0x53,0x46,0x48,0x00,0x00,0xAA,0x11,
    0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC
  };
  UINT8 block[512] __attribute__((aligned(512)));
  UINT8 hfs_uuid[8];
  UINT64 part_lba = 0;
  UINT64 entries_lba;
  UINT32 entry_size;
  UINT32 entry_count;
  UINT32 media_id;

  if (!bio || !bio->MediaInfo || bio->MediaInfo->BlockSize != 512)
    return FALSE;

  media_id = bio->MediaInfo->MediaId;
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, 1, sizeof(block), block)))
    return FALSE;
  if (CompareMem(block, "EFI PART", 8) != 0)
    return FALSE;

  entries_lba = rd64le(block + 72);
  entry_count = rd32le(block + 80);
  entry_size = rd32le(block + 84);
  if (entry_size < 128 || entry_size > 512 || entry_count == 0)
    return FALSE;

  for (UINT32 idx = 0; idx < entry_count && idx < 128; idx++) {
    UINT64 lba = entries_lba + ((UINT64)idx * entry_size) / 512;
    UINTN off = (UINTN)(((UINT64)idx * entry_size) % 512);
    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, lba, sizeof(block), block)))
      return FALSE;
    if (!guid_eq_raw(block + off, apple_hfs_guid))
      continue;
    part_lba = rd64le(block + off + 32);
    break;
  }

  if (part_lba == 0)
    return FALSE;

  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, part_lba + 2, sizeof(block), block)))
    return FALSE;
  if (rd16be(block) != 0x482B && rd16be(block) != 0x4858)
    return FALSE;

  CopyMem(hfs_uuid, block + 104, sizeof(hfs_uuid));
  hfs_uuid_from_finder_info(hfs_uuid, uuid_str);
  log_info(L"DT: found HFS boot UUID %a at GPT LBA %lu\r\n", uuid_str, part_lba);
  return TRUE;
}

static BOOLEAN find_hfs_boot_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  static EFI_GUID block_io_guid =
      { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;
  EFI_STATUS status;

  status = uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5,
                             ByProtocol, &block_io_guid, NULL, &count, &handles);
  if (EFI_ERROR(status))
    return FALSE;

  for (UINTN i = 0; i < count; i++) {
    XnuBlockIoProtocol *bio = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                    handles[i], &block_io_guid, (VOID **)&bio)))
      continue;
    if (read_hfs_uuid_from_blockio(bio, uuid_str)) {
      uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
      return TRUE;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return FALSE;
}

static BOOLEAN read_ext4_uuid_from_blockio(XnuBlockIoProtocol *bio, CHAR8 uuid_str[37]) {
  static const UINT8 linux_fs_guid[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,
    0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
  };
  UINT8 block[512] __attribute__((aligned(512)));
  UINT64 entries_lba;
  UINT32 entry_size, entry_count, media_id;
  UINT64 part_lba = 0;

  if (!bio || !bio->MediaInfo || bio->MediaInfo->BlockSize != 512)
    return FALSE;

  media_id = bio->MediaInfo->MediaId;
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, 1, sizeof(block), block)))
    return FALSE;
  if (CompareMem(block, "EFI PART", 8) != 0)
    return FALSE;

  entries_lba = rd64le(block + 72);
  entry_count = rd32le(block + 80);
  entry_size = rd32le(block + 84);
  if (entry_size < 128 || entry_size > 512 || entry_count == 0)
    return FALSE;

  for (UINT32 idx = 0; idx < entry_count && idx < 128; idx++) {
    UINT64 lba = entries_lba + ((UINT64)idx * entry_size) / 512;
    UINTN off = (UINTN)(((UINT64)idx * entry_size) % 512);
    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, lba, sizeof(block), block)))
      return FALSE;
    if (!guid_eq_raw(block + off, linux_fs_guid))
      continue;
    part_lba = rd64le(block + off + 32);
    break;
  }

  if (part_lba == 0)
    return FALSE;

  /* superblock at partition byte 1024 == LBA part_lba + 2 */
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, part_lba + 2, sizeof(block), block)))
    return FALSE;
  if ((UINT16)(block[0x38] | ((UINT16)block[0x39] << 8)) != 0xEF53)
    return FALSE;

  fmt_uuid_bytes(block + 0x68, uuid_str);
  log_info(L"DT: found ext4 boot UUID %a at GPT LBA %lu\r\n", uuid_str, part_lba);
  return TRUE;
}

static UINTN dp_total_size(EFI_DEVICE_PATH *dp) {
  EFI_DEVICE_PATH *n = dp;
  UINTN len = 0;
  while (n && !(n->Type == 0x7F)) {
    UINTN nlen = (UINTN)n->Length[0] | ((UINTN)n->Length[1] << 8);
    if (nlen < 4) break;                 /* malformed; stop rather than spin */
    len += nlen;
    n = (EFI_DEVICE_PATH *)((UINT8 *)n + nlen);
  }
  return len;
}

static BOOLEAN dp_is_boot_disk(AppContext *ctx, EFI_HANDLE cand) {
  static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  EFI_DEVICE_PATH *boot_dp = NULL, *cand_dp = NULL;

  if (!ctx->boot_volume) return FALSE;
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                  ctx->boot_volume, &dp_guid, (VOID **)&boot_dp)))
    return FALSE;
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                  cand, &dp_guid, (VOID **)&cand_dp)))
    return FALSE;

  UINTN cand_len = dp_total_size(cand_dp);
  if (cand_len == 0 || cand_len > dp_total_size(boot_dp)) return FALSE;

  /* The disk handle's path must be a proper prefix of the ESP's path. */
  for (UINTN i = 0; i < cand_len; i++) {
    if (((UINT8 *)cand_dp)[i] != ((UINT8 *)boot_dp)[i]) return FALSE;
  }
  return TRUE;
}

/*
 * The CONTAINER uuid is used, not a volume uuid, because it sits at a fixed
 * offset in a single block; reaching a volume superblock would mean walking
 * the checkpoint ring and an object map here in the loader.
 */
static BOOLEAN read_apfs_uuid_from_blockio(XnuBlockIoProtocol *bio, CHAR8 uuid_str[37]) {
  UINT8 block[512] __attribute__((aligned(512)));
  UINT64 entries_lba;
  UINT32 entry_size, entry_count, media_id;

  if (!bio || !bio->MediaInfo || bio->MediaInfo->BlockSize != 512)
    return FALSE;

  media_id = bio->MediaInfo->MediaId;
  if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                  bio, media_id, 1, sizeof(block), block)))
    return FALSE;
  if (CompareMem(block, "EFI PART", 8) != 0)
    return FALSE;

  entries_lba = rd64le(block + 72);
  entry_count = rd32le(block + 80);
  entry_size = rd32le(block + 84);
  if (entry_size < 128 || entry_size > 512 || entry_count == 0)
    return FALSE;

  for (UINT32 idx = 0; idx < entry_count && idx < 128; idx++) {
    UINT64 lba = entries_lba + ((UINT64)idx * entry_size) / 512;
    UINTN off = (UINTN)(((UINT64)idx * entry_size) % 512);
    UINT64 part_lba;

    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, lba, sizeof(block), block)))
      return FALSE;
    part_lba = rd64le(block + off + 32);
    if (part_lba == 0)
      continue;                          /* unused entry */

    if (EFI_ERROR(uefi_call_wrapper(bio->ReadBlocks, 5,
                                    bio, media_id, part_lba, sizeof(block), block)))
      continue;
    if (CompareMem(block + 32, "NXSB", 4) != 0)
      continue;

    fmt_uuid_bytes(block + 72, uuid_str);
    log_info(L"DT: found APFS container UUID %a at GPT LBA %lu\r\n", uuid_str, part_lba);
    return TRUE;
  }
  return FALSE;
}

static BOOLEAN find_apfs_boot_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  static EFI_GUID block_io_guid =
      { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;

  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5,
                                  ByProtocol, &block_io_guid, NULL, &count, &handles)))
    return FALSE;

  /* Prefer the disk we booted from, as the ext4 scan does. */
  for (UINTN pass = 0; pass < 2; pass++) {
    for (UINTN i = 0; i < count; i++) {
      XnuBlockIoProtocol *bio = NULL;
      BOOLEAN on_boot_disk = dp_is_boot_disk(ctx, handles[i]);

      if ((pass == 0) != (on_boot_disk != FALSE))
        continue;
      if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                      handles[i], &block_io_guid, (VOID **)&bio)))
        continue;
      if (read_apfs_uuid_from_blockio(bio, uuid_str)) {
        uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
        return TRUE;
      }
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return FALSE;
}

static BOOLEAN find_ext4_boot_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  static EFI_GUID block_io_guid =
      { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
  EFI_HANDLE *handles = NULL;
  UINTN count = 0;

  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->LocateHandleBuffer, 5,
                                  ByProtocol, &block_io_guid, NULL, &count, &handles)))
    return FALSE;

  for (UINTN pass = 0; pass < 2; pass++) {
    for (UINTN i = 0; i < count; i++) {
      XnuBlockIoProtocol *bio = NULL;
      BOOLEAN on_boot_disk = dp_is_boot_disk(ctx, handles[i]);

      if (pass == 0 && !on_boot_disk) continue;
      if (pass == 1 && on_boot_disk) continue;   /* already tried */

      if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                      handles[i], &block_io_guid, (VOID **)&bio)))
        continue;
      if (read_ext4_uuid_from_blockio(bio, uuid_str)) {
        log_info(L"DT: ext4 root found on %a disk\r\n",
                 pass == 0 ? "boot" : "non-boot (fallback)");
        uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
        return TRUE;
      }
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, handles);
  return FALSE;
}

/* Try to extract a GPT partition UUID from the boot volume's device path.
 * Looks for a HARDDRIVE_DEVICE_PATH (Type=4, SubType=1) with SignatureType=2.
 * Returns TRUE and fills uuid_str[37] on success. */
static BOOLEAN dp_get_partition_uuid(AppContext *ctx, CHAR8 uuid_str[37]) {
  if (!ctx->boot_volume) return FALSE;
  static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
  EFI_DEVICE_PATH *dp = NULL;
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                   ctx->boot_volume, &dp_guid, (VOID **)&dp)))
    return FALSE;
  EFI_DEVICE_PATH *n = dp;
  while (!(n->Type == 0x7F && n->SubType == 0xFF)) {
    UINT32 nlen = (UINT32)(n->Length[0] | ((UINT32)n->Length[1] << 8));
    /* HARDDRIVE_DEVICE_PATH: Type=4, SubType=1, Length=42 */
    if (n->Type == 4 && n->SubType == 1 && nlen >= 42) {
      UINT8 *hd = (UINT8 *)n;
      UINT8 sig_type = hd[41];
      if (sig_type == 2) { /* GPT GUID at hd[24..39] */
        fmt_guid_uuid(hd + 24, uuid_str);
        return TRUE;
      }
    }
    n = (EFI_DEVICE_PATH *)((UINT8 *)n + nlen);
  }
  return FALSE;
}

#if defined(XNU_LOADER_PLATFORM_SC8280XP)
/* Interrupt controllers the kernel finds by phandle: the GIC (/arm-io/gic, one cell, the
 * INTID) and the TLMM (two cells, <pin flags>) */
#define SC8280XP_GIC_PHANDLE    0x7ff00002
#define SC8280XP_TLMM_PHANDLE   0x7ff00003
#define DT_IRQ_LEVEL_LOW        8

/* The X13s keyboard, TrackPoint and touchpad bus: GENI serial engine i2c@894000
 * and its hid-over-i2c slaves, and the TLMM pins their interrupt lines are on. 
 * The platform expert publishes a nub for each child of a simple-bus. reg values 
 * are CPU physical addresses. */
static DeviceTreeNode *sc8280xp_soc(AppContext *ctx) {
  static const struct {
    const CHAR8 *name;
    UINT32 addr, descr, pin;
  } slaves[] = {
    { "keyboard@68", 0x68, 0x0001, 104 },
    { "touchpad@15", 0x15, 0x0001, 182 },
    { "touchpad@2c", 0x2c, 0x0020, 182 }, // second source, not fitted on every unit
  };

  DeviceTreeNode *soc = dt_create_node(ctx);
  DeviceTreeNode *tlmm = dt_create_node(ctx);
  DeviceTreeNode *i2c = dt_create_node(ctx);
  UINT64 tlmm_reg[2] = { 0xf100000ULL, 0x300000ULL };
  UINT32 tlmm_reserved[10] = { 70, 2, 74, 6, 125, 2, 128, 2, 154, 4 };
  UINT64 i2c_reg[2] = { 0x894000ULL, 0x4000ULL };

  dt_prop_str(ctx, soc, "name", "soc");
  dt_prop_str(ctx, soc, "compatible", "simple-bus");
  dt_prop_u32(ctx, soc, "#address-cells", 2);
  dt_prop_u32(ctx, soc, "#size-cells", 2);

  /* summary interrupt: SPI 208, INTID 240 */
  dt_prop_str(ctx, tlmm, "name", "pinctrl@f100000");
  dt_prop_str(ctx, tlmm, "compatible", "qcom,sc8280xp-tlmm");
  dt_prop(ctx, tlmm, "reg", tlmm_reg, sizeof(tlmm_reg));
  dt_prop_u32(ctx, tlmm, "ngpios", 230);
  dt_prop(ctx, tlmm, "gpio-reserved-ranges", tlmm_reserved, sizeof(tlmm_reserved));
  dt_prop(ctx, tlmm, "interrupt-controller", NULL, 0);
  dt_prop_u32(ctx, tlmm, "#interrupt-cells", 2);
  dt_prop_u32(ctx, tlmm, "AAPL,phandle", SC8280XP_TLMM_PHANDLE);
  dt_prop_u32(ctx, tlmm, "interrupt-parent", SC8280XP_GIC_PHANDLE);
  dt_prop_u32(ctx, tlmm, "interrupts", 240);
  dt_add_child(ctx, soc, tlmm);

  dt_prop_str(ctx, i2c, "name", "i2c@894000");
  dt_prop_str(ctx, i2c, "compatible", "qcom,geni-i2c");
  dt_prop(ctx, i2c, "reg", i2c_reg, sizeof(i2c_reg));
  dt_prop_u32(ctx, i2c, "clock-frequency", 400000);
  dt_prop_u32(ctx, i2c, "#address-cells", 1);
  dt_prop_u32(ctx, i2c, "#size-cells", 0);
  dt_prop_u32(ctx, i2c, "interrupt-parent", SC8280XP_GIC_PHANDLE);
  dt_prop_u32(ctx, i2c, "interrupts", 619);
  for (UINTN i = 0; i < sizeof(slaves) / sizeof(slaves[0]); i++) {
    DeviceTreeNode *hid = dt_create_node(ctx);
    dt_prop_str(ctx, hid, "name", slaves[i].name);
    dt_prop_str(ctx, hid, "compatible", "hid-over-i2c");
    dt_prop_u32(ctx, hid, "reg", slaves[i].addr);
    dt_prop_u32(ctx, hid, "hid-descr-addr", slaves[i].descr);
    {
      UINT32 irq[2] = { slaves[i].pin, DT_IRQ_LEVEL_LOW };
      dt_prop_u32(ctx, hid, "interrupt-parent", SC8280XP_TLMM_PHANDLE);
      dt_prop(ctx, hid, "interrupts", irq, sizeof(irq));
    }
    dt_add_child(ctx, i2c, hid);
  }
  dt_add_child(ctx, soc, i2c);
  return soc;
}
#endif

EFI_STATUS dt_build(
    AppContext *ctx,
    VOID **out_blob,
    UINT32 *out_size,
    LowMemBuffer *out_buf,
    const CHAR8 *boot_args,
    UINT64 rt_table_phys)
{
  if (!ctx || !out_blob || !out_size || !out_buf || !boot_args)
    return EFI_INVALID_PARAMETER;

  /* Allocate 8 KB for the DT blob in the fixed boot-info block (>= 0x100000 so
   * it survives XNU's pmap_lowmem_finalize; see boot.h XNU_BOOTINFO_BASE). */
  EFI_PHYSICAL_ADDRESS dt_addr = XNU_DEVTREE_PHYS;
  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress,
      EfiLoaderData,
      XNU_DEVTREE_PAGES,
      &dt_addr);
  if (EFI_ERROR(status)) {
    log_info(L"DT: AllocatePages failed: %r\n", status);
    return status;
  }

  UINTN dt_size = EFI_PAGE_SIZE * XNU_DEVTREE_PAGES;
  UINT8 *dt_base = (UINT8 *)(UINTN)dt_addr;
  SetMem(dt_base, dt_size, 0);

  DeviceTreeNode *chosen = dt_create_node(ctx);
  /* Kept at function scope so the /options platform-uuid fallback below can
   * reuse it; the uuid_str that computes it is scoped to an inner block. */
  CHAR8 boot_uuid_str[37];
  boot_uuid_str[0] = '\0';

  dt_prop_str(ctx, chosen, "name", "chosen");
  dt_prop_str(ctx, chosen, "boot-args", boot_args);

  /* Carry the genuine GOP byte order and aperture in an additive property.
   * PE_Video has no firmware-format field; the native driver cross-checks this
   * metadata against its actual boot console before exposing color masks. */
  {
    boot_video_info video;
    EFI_STATUS video_status = boot_probe_video(ctx, boot_args, &video);
    if (!EFI_ERROR(video_status) && video.found) {
      struct native_boot_video metadata = {
        NATIVE_BOOT_VIDEO_VERSION, video.pixel_format, video.base_addr,
        video.aperture_length, (UINT32)video.width, (UINT32)video.height,
        (UINT32)video.row_bytes, (UINT32)video.depth
      };
      dt_prop(ctx, chosen, NATIVE_BOOT_VIDEO_PROPERTY, &metadata, sizeof(metadata));
    }
  }

  /* kmutil panics "failed to get manifest properties" without /chosen/manifest-properties,
   * then "non-sensical crypto hash method" without crypto-hash-method. */
  {
    DeviceTreeNode *manifest = dt_create_node(ctx);
    dt_prop_str(ctx, manifest, "name", "manifest-properties");
    dt_prop_str(ctx, manifest, "crypto-hash-method", "sha2-384");
    dt_add_child(ctx, chosen, manifest);
    dt_prop_str(ctx, chosen, "crypto-hash-method", "sha2-384");
  }

  /* arm64 IOPlatformExpertDevice::generatePlatformUUID hashes these two; without them
   * IOPlatformUUID is never published and gethostuuid (opendirectoryd) blocks forever. */
  {
    static const UINT8 unique_chip_id[8] = { 0x50, 0x44, 0x56, 0x4d, 0x00, 0x00, 0x00, 0x01 };
    dt_prop(ctx, chosen, "unique-chip-id", unique_chip_id, sizeof(unique_chip_id));
    dt_prop_u32(ctx, chosen, "chip-id", 0xfe00);
  }

  /* arm64 xnu (CONFIG_CSR_FROM_DT) takes its SIP configuration from the local
   * boot policy at /chosen/asmb: lp-sip0 is the csr_config word, lp-sip1 the
   * "unauthenticated root allowed" bit (kern_csr.c csr_bootstrap). */
  {
    UINT32 csr;
    if (boot_cmdline_csr_config(boot_args, &csr)) {
      DeviceTreeNode *asmb = dt_create_node(ctx);
      dt_prop_str(ctx, asmb, "name", "asmb");
      dt_prop_u64(ctx, asmb, "lp-sip0", csr);
      dt_prop_u32(ctx, asmb, "lp-sip1", (csr & (1u << 11)) ? 1 : 0);
      dt_add_child(ctx, chosen, asmb);
    }
  }

  /* IODTNVRAM obtains its backing-store geometry from /chosen. xnu 12377 wants
   * the bank triple, not just the old nvram-total-size, and
   * IODTNVRAMFormatHandler::getNVRAMProperties fails the whole handler if any
   * one is missing - which reaches XNU as a bare
   * "IONVRAMCHRPHandler creation failed" panic. nvram-proxy-data must be
   * exactly nvram-bank-size bytes or CHRP init rejects the length. A blank
   * image is fine: it fails isValidImage (so we get the CHRP handler, not V3)
   * and unserializeImage only logs. */
  {
    static UINT8 nvram_blank[XNU_NVRAM_BANK_SIZE];
    chrp_build_blank_bank(nvram_blank, sizeof(nvram_blank));
    dt_prop_u32(ctx, chosen, "nvram-total-size",   XNU_NVRAM_BANK_SIZE);
    dt_prop_u32(ctx, chosen, "nvram-bank-size",    XNU_NVRAM_BANK_SIZE);
    dt_prop_u32(ctx, chosen, "nvram-bank-count",   1);
    dt_prop_u32(ctx, chosen, "nvram-current-bank", 0);
    dt_prop(ctx, chosen, "nvram-proxy-data", nvram_blank, sizeof(nvram_blank));
  }

  /* ml_unsafe_kernel_text_init() (osfmk/arm64/machine_routines.c) reads this
   * and leaves _unsafe_kernel_text_initialized false when it is absent, so
   * ml_unsafe_kernel_text() then trips its own assert on DEVELOPMENT/DEBUG.
   * Zero means "CTRR will not be enabled", which is the truth here: QEMU virt
   * has no CTRR/KTRR and the board is built NO_MONITOR. */
  dt_prop_u32(ctx, chosen, "kernel-ctrr-to-be-enabled", 0);

  /* boot-uuid / apfs-preboot-uuid: prefer an explicit root UUID.  The EFI
   * image is loaded from the ESP, so the boot-volume device path is usually
   * not the Darwin root volume and must not become a strict root match. */
  {
    CHAR8 uuid_str[37];
    BOOLEAN got_uuid = boot_arg_get_uuid(boot_args, "boot-uuid", uuid_str) ||
                       boot_arg_get_uuid(boot_args, "root-uuid", uuid_str);
    BOOLEAN got_hfs_uuid = FALSE;
    BOOLEAN got_ext4_uuid = FALSE;
    BOOLEAN got_apfs_uuid = FALSE;
    BOOLEAN got_boot_volume_uuid = FALSE;

    if (!got_uuid) {
      got_hfs_uuid = find_hfs_boot_uuid(ctx, uuid_str);
    }

    /* No HFS volume found: try an ext4 root (Ext4FileSystemDriver publishes
     * boot-uuid-media for it, mirroring AppleFileSystemDriver for HFS). */
    /* No firmware disk (the Linux-image entry): ext4-uuid names the filesystem, and an
     * MBR partition has no GPT UUID for a strict root match to find */
    if (!got_uuid && !got_hfs_uuid) {
      got_ext4_uuid = boot_arg_get_uuid(boot_args, "ext4-uuid", uuid_str) ||
                      find_ext4_boot_uuid(ctx, uuid_str);
    }

    /* Then an APFS container root (ApfsFileSystemDriver publishes
     * boot-uuid-media for it, matching nx_uuid from block zero). */
    /* No firmware disk (the Linux-image entry): the container UUID can come
     * from the boot-args and is then treated as if read from block zero */
    if (!got_uuid && !got_hfs_uuid && !got_ext4_uuid) {
      got_apfs_uuid = boot_arg_get_uuid(boot_args, "apfs-container-uuid", uuid_str);
      // IOUUIDMatching compares exactly and APFS media publish their uuids in upper case
      for (UINTN _c = 0; got_apfs_uuid && _c < 36; _c++)
        if (uuid_str[_c] >= 'a' && uuid_str[_c] <= 'f') uuid_str[_c] -= 'a' - 'A';
      got_apfs_uuid = got_apfs_uuid || find_apfs_boot_uuid(ctx, uuid_str);
    }

    if (!got_uuid && !got_hfs_uuid && !got_ext4_uuid && !got_apfs_uuid) {
      got_boot_volume_uuid = dp_get_partition_uuid(ctx, uuid_str);
    }

    if (!got_uuid && !got_hfs_uuid && !got_ext4_uuid && !got_apfs_uuid &&
        !got_boot_volume_uuid) {
      CHAR8 fallback[] = "dd5c6498-90d9-4b35-a95f-1944ebc01791";
      for (UINTN _i = 0; _i < 37; _i++) uuid_str[_i] = fallback[_i];
    }
    dt_prop_str(ctx, chosen, "boot-uuid", uuid_str);
    for (UINTN _u = 0; _u < 37; _u++) boot_uuid_str[_u] = uuid_str[_u];
    dt_prop_str(ctx, chosen, "apfs-preboot-uuid", uuid_str);

    /* root-matching: UUID-based only for an explicit root UUID.  For an HFS
     * filesystem UUID discovered from disk, let IOKitBSDInit publish boot-uuid
     * and let AppleFileSystemDriver publish boot-uuid-media after it verifies
     * the filesystem UUID. IOMedia's UUID is the GPT partition UUID, not the
     * HFS filesystem UUID. */
    if (got_uuid) {
      CHAR8 rm[256];
      CHAR8 *p = rm;
      /* Build: <dict><key>IOProviderClass</key><string>IOMedia</string>
                <key>IOPropertyMatch</key><dict><key>UUID</key><string>UUID</string></dict></dict> */
      static const CHAR8 rm_a[] = "<dict><key>IOProviderClass</key><string>IOMedia</string>"
                                   "<key>IOPropertyMatch</key><dict><key>UUID</key><string>";
      static const CHAR8 rm_b[] = "</string></dict></dict>";
      UINTN la = 0; while (rm_a[la]) la++;
      UINTN lb = 0; while (rm_b[lb]) lb++;
      for (UINTN _i = 0; _i < la; _i++) *p++ = rm_a[_i];
      for (UINTN _i = 0; _i < 36; _i++) *p++ = uuid_str[_i];
      for (UINTN _i = 0; _i < lb; _i++) *p++ = rm_b[_i];
      *p++ = '\0';
      dt_prop_str(ctx, chosen, "root-matching", rm);
    } else {
      dt_prop_str(ctx, chosen, "root-matching",
          "<dict><key>IOProviderClass</key><string>IOMedia</string>"
          "<key>IOPropertyMatch</key><dict><key>Whole Media</key></dict></dict>");
    }
  }

  {
    CHAR8 volgroup_uuid[37];
    if (boot_arg_get_uuid(boot_args, "associated-volume-group", volgroup_uuid) ||
        boot_arg_get_uuid(boot_args, "volgroup-uuid", volgroup_uuid)) {
      dt_prop_str(ctx, chosen, "associated-volume-group", volgroup_uuid);
    }
  }

  {
    CHAR8 boot_objects_path[256];
    if (boot_arg_get_value(boot_args, "boot-objects-path",
                           boot_objects_path, sizeof(boot_objects_path))) {
      dt_prop_str(ctx, chosen, "boot-objects-path", boot_objects_path);
    }
  }

  /* boot-file: path to the kernel on the boot volume. */
  dt_prop_str(ctx, chosen, "boot-file", "\\EFI\\BOOT\\kernel");

  /* boot-device-path: full serialized EFI device path of the boot volume.
   * boot-file-path:   same path + FilePath media node for the kernel file.
   * Fall back to a 4-byte end-of-path node if the volume handle is absent. */
  {
    static const UINT8 devpath_end[4] = { 0x7F, 0xFF, 0x04, 0x00 };
    static EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

    EFI_DEVICE_PATH *vol_dp = NULL;
    UINT32 vol_dp_len = 0;   /* bytes up to and including end node */

    if (ctx->boot_volume &&
        !EFI_ERROR(uefi_call_wrapper(ctx->bs->HandleProtocol, 3,
                                     ctx->boot_volume, &dp_guid,
                                     (VOID **)&vol_dp)) && vol_dp) {
      /* Measure length: walk nodes until end node, add 4 for the end node */
      EFI_DEVICE_PATH *n = vol_dp;
      while (!(n->Type == 0x7F && n->SubType == 0xFF)) {
        UINT32 nlen = (UINT32)(n->Length[0] | ((UINT32)n->Length[1] << 8));
        vol_dp_len += nlen;
        n = (EFI_DEVICE_PATH *)((UINT8 *)n + nlen);
      }
      vol_dp_len += 4; /* include end node */
    }

    /* boot-device-path */
    if (vol_dp_len >= 4) {
      dt_prop(ctx, chosen, "boot-device-path", (VOID *)vol_dp, vol_dp_len);
    } else {
      dt_prop(ctx, chosen, "boot-device-path", (VOID *)devpath_end, 4);
    }

    /* boot-file-path: volume path (minus its end node) + FilePath node + end */
    {
      static const CHAR16 kpath[] = L"\\EFI\\BOOT\\kernel";
      /* kpath has 16 chars + null = 17 * 2 = 34 bytes; node = 4 + 34 = 38 */
      UINT32 fp_node_len = (UINT32)(4 + (16 + 1) * 2);
      UINT32 vol_prefix  = (vol_dp_len >= 4) ? (vol_dp_len - 4) : 0;
      UINT32 file_dp_len = vol_prefix + fp_node_len + 4;

      UINT8 *file_dp_buf = NULL;
      if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3,
                                       EfiBootServicesData, file_dp_len,
                                       (VOID **)&file_dp_buf))) {
        /* Copy volume path (without its end node) */
        if (vol_prefix)
          CopyMem(file_dp_buf, vol_dp, vol_prefix);

        /* Build FilePath node at offset vol_prefix */
        UINT8 *fp = file_dp_buf + vol_prefix;
        fp[0] = 4;   /* Type: Media */
        fp[1] = 4;   /* SubType: File Path */
        fp[2] = (UINT8)(fp_node_len & 0xFF);
        fp[3] = (UINT8)(fp_node_len >> 8);
        CopyMem(fp + 4, kpath, (16 + 1) * 2);

        /* End node */
        UINT8 *ep = fp + fp_node_len;
        ep[0] = 0x7F; ep[1] = 0xFF; ep[2] = 0x04; ep[3] = 0x00;

        dt_prop(ctx, chosen, "boot-file-path", (VOID *)file_dp_buf, file_dp_len);
        uefi_call_wrapper(ctx->bs->FreePool, 1, file_dp_buf);
      } else {
        dt_prop(ctx, chosen, "boot-file-path", (VOID *)devpath_end, 4);
      }
    }
  }

  /* boot-kernelcache-adler32: Adler-32 of the prelinked kernel.
   * Zero when booting without a prelinked kernel cache. */
  {
    UINT32 adler = 0;
    dt_prop(ctx, chosen, "boot-kernelcache-adler32", &adler, 4);
  }

  /* Entropy bytes for PE_get_random_seed(). Use firmware RNG or the fresh
   * host seed injected into a private ESP on each emulator launch. */
  {
    /* 256 bytes: arm64 xnu asks for 4*SHA512_DIGEST_LENGTH and panics in
     * bootseed_init_bootloader if it gets less. x86_64 only reads 64. */
    UINT8 seed[256];
    static EFI_GUID rng_guid = EFI_RNG_PROTOCOL_GUID;
    EFI_RNG_PROTOCOL *rng = NULL;
    BOOLEAN got_rng = FALSE;

    if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->LocateProtocol, 3,
                                     &rng_guid, NULL, (VOID **)&rng)) && rng) {
      /* MUST go through uefi_call_wrapper: firmware protocol methods use the
       * MS x64 ABI, our code is SysV. A direct rng->GetRNG(...) call passes
       * args in the wrong registers -> firmware faults. QEMU/OVMF has no RNG
       * protocol so this path only ever ran on real hardware (where it crashed
       * right after "found HFS boot UUID"). */
      if (!EFI_ERROR(uefi_call_wrapper(rng->GetRNG, 4, rng, NULL, sizeof(seed), seed)))
        got_rng = TRUE;
    }

    if (!got_rng) {
      EFI_FILE_PROTOCOL *volume = NULL;
      FileBuffer host_seed = {0};
      EFI_STATUS status = app_open_self_volume(ctx, &volume);
      if (!EFI_ERROR(status)) {
        status = file_read_all(ctx, volume, L"\\EFI\\BOOT\\entropy.bin", &host_seed);
        uefi_call_wrapper(volume->Close, 1, volume);
      }
      if (EFI_ERROR(status) || host_seed.size != sizeof(seed)) {
        file_free(ctx, &host_seed);
        log_error(L"Missing 256-byte per-boot entropy and firmware RNG\r\n");
        return EFI_NOT_FOUND;
      }
      CopyMem(seed, host_seed.data, sizeof(seed));
      volatile UINT8 *staging_seed = (volatile UINT8 *)host_seed.data;
      for (UINTN si = 0; si < sizeof(seed); ++si) staging_seed[si] = 0;
      file_free(ctx, &host_seed);
    }

    dt_prop(ctx, chosen, "random-seed", seed, sizeof(seed));
    volatile UINT8 *stack_seed = seed;
    for (UINTN si = 0; si < sizeof(seed); ++si) stack_seed[si] = 0;
  }

  /* booter-name: boot.efi writes "boot.efi" (9 bytes incl null) */
  dt_prop_str(ctx, chosen, "booter-name", "boot.efi");

  static const CHAR8 binfo[] = "boot.efi";
  dt_prop_str(ctx, chosen, "booter-build-info", binfo);

  /* machine-signature: 4 bytes from FACS.HardwareSignature.
   * boot.efi reads this from the FACP ACPI table's FACS pointer.
   * OVMF's FACS hardware signature is 0. */
  {
    UINT32 sig = 0;
    dt_prop_u32(ctx, chosen, "machine-signature", sig);
  }

  /* IOScreenLockState / IOFDEUserMatched: boot.efi sets these on /chosen in
   * its handoff path from the CoreStorage/FileVault (FDE) unlock state.
   * IOKit (IOHibernateSystemPostWake / login window) reads them.  For a plain
   * un-encrypted boot with no FileVault the faithful values are
   * kIOScreenLockNoLock (1) and "no user matched" (0). */
  {
    UINT32 io_screen_lock_state = 1; /* kIOScreenLockNoLock */
    UINT32 io_fde_user_matched  = 0;
    dt_prop_u32(ctx, chosen, "IOScreenLockState", io_screen_lock_state);
    dt_prop_u32(ctx, chosen, "IOFDEUserMatched", io_fde_user_matched);
  }

  typedef struct {
    UINT64 paddr;
    UINT64 length;
  } MemoryMapFileInfo;

  DeviceTreeNode *memory_map = NULL;

#if defined(__aarch64__)
  /*
   * dram-base / dram-size: the *actual* physical RAM base/size as iBoot
   * would report it - distinct from boot_args physBase/memSize, which only
   * cover kernel-managed memory. arm_init() (osfmk/arm/arm_init.c) reads
   * these unconditionally and panics if either is missing; gDramBase/
   * gDramSize feed real_avail_end/pmap accounting downstream.
   */
  {
    UINT64 dram_base = XNU_LOADER_RAM_BASE;
    /* The DRAM address extent includes reserved gaps; a sum of usable
     * bytes misclassifies upper RAM pages as device memory in XNU. */
    UINT64 dram_size = app_detect_physical_memory_extent(ctx, dram_base);
    log_info(L"DT: DRAM extent base=0x%lx size=0x%lx\r\n", dram_base, dram_size);
    dt_prop(ctx, chosen, "dram-base", &dram_base, sizeof(dram_base));
    dt_prop(ctx, chosen, "dram-size", &dram_size, sizeof(dram_size));
  }

  {
    typedef struct {
      UINT32 version;
      UINT8  uuid[16];
      UINT32 num_entries;
    } TrustCacheModule1Empty;

    EFI_PHYSICAL_ADDRESS tc_phys = ctx->trustcache_phys;
    EFI_STATUS tc_status = EFI_SUCCESS;
    if (tc_phys == 0) {
      tc_phys = XNU_TRUSTCACHE_PHYS;
      tc_status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
          AllocateAddress, EfiLoaderData, 1, &tc_phys);
    }
    if (!EFI_ERROR(tc_status) && ctx->trustcache_data != NULL) {
      // Apple's trust caches from \trustcache.bin, already in the offsets header + modules layout
      CopyMem((VOID *)(UINTN)tc_phys, ctx->trustcache_data, ctx->trustcache_size);

      if (memory_map == NULL) {
        memory_map = dt_create_node(ctx);
        dt_prop_str(ctx, memory_map, "name", "memory-map");
      }

      MemoryMapFileInfo tc_info;
      tc_info.paddr = (UINT64)tc_phys;
      tc_info.length = ctx->trustcache_size;
      dt_prop(ctx, memory_map, "TrustCache", &tc_info, sizeof(tc_info));

      log_info(L"DT: TrustCache at 0x%lx, %lu bytes\r\n", (UINT64)tc_phys, ctx->trustcache_size);
    } else if (!EFI_ERROR(tc_status)) {
      TrustCacheModule1Empty *tc = (TrustCacheModule1Empty *)(UINTN)tc_phys;
      SetMem(tc, EFI_PAGE_SIZE, 0);
      tc->version = 1;
      tc->num_entries = 0;

      if (memory_map == NULL) {
        memory_map = dt_create_node(ctx);
        dt_prop_str(ctx, memory_map, "name", "memory-map");
      }

      MemoryMapFileInfo tc_info;
      tc_info.paddr = (UINT64)tc_phys;
      tc_info.length = sizeof(TrustCacheModule1Empty);
      dt_prop(ctx, memory_map, "TrustCache", &tc_info, sizeof(tc_info));

      log_info(L"DT: empty static TrustCache at 0x%lx\r\n", (UINT64)tc_phys);
    } else {
      log_info(L"DT: TrustCache page alloc failed: %r (skipping)\r\n", tc_status);
    }
  }
#endif
#if defined(__riscv)
  // the kernel panics without dram-base and dram-size, the bank that holds the kernel
  {
    UINT64 dram_base = ctx->dram_base;
    UINT64 dram_size = ctx->dram_size;
    dt_prop(ctx, chosen, "dram-base", &dram_base, sizeof(dram_base));
    dt_prop(ctx, chosen, "dram-size", &dram_size, sizeof(dram_size));
  }
  if (ctx->fdt_copy_size != 0) {
    if (memory_map == NULL) {
      memory_map = dt_create_node(ctx);
      dt_prop_str(ctx, memory_map, "name", "memory-map");
    }
    MemoryMapFileInfo fdt_info;
    fdt_info.paddr = (UINT64)ctx->fdt_copy_phys;
    fdt_info.length = ctx->fdt_copy_size;
    dt_prop(ctx, memory_map, "FDT", &fdt_info, sizeof(fdt_info));
  }
#endif

  if (ctx->ramdisk_size != 0) {
    if (memory_map == NULL) {
      memory_map = dt_create_node(ctx);
      dt_prop_str(ctx, memory_map, "name", "memory-map");
    }
    MemoryMapFileInfo ramdisk_info;
    ramdisk_info.paddr = (UINT64)ctx->ramdisk_phys;
    ramdisk_info.length = ctx->ramdisk_size;
    dt_prop(ctx, memory_map, "RAMDisk", &ramdisk_info, sizeof(ramdisk_info));
    log_info(L"DT: RAMDisk at 0x%lx size=0x%lx\r\n",
             (UINT64)ctx->ramdisk_phys, ctx->ramdisk_size);
  }

  if (memory_map != NULL)
    dt_add_child(ctx, chosen, memory_map);

  DeviceTreeNode *platform = dt_create_node(ctx);
  dt_prop_str(ctx, platform, "name", "platform");

  UINT64 initial_tsc = 0;
  UINT64 tsc_frequency = estimate_tsc_frequency(ctx, &initial_tsc);

  ctx->fsb_frequency = 100000000ULL;
#if defined(PD_ARCH_X86)
  /* Latest XNU derives Penryn's TSC from FSBFrequency and the actual MSR
   * ratio; it does not consume TSCFrequency. QEMU's synthetic ratio need
   * not match a physical 100 MHz BCLK, so calibrate its bus from EFI time. */
  UINT64 calibrated_bus = penryn_bus_frequency(tsc_frequency);
  if (calibrated_bus)
    ctx->fsb_frequency = calibrated_bus;
#endif
  dt_prop_u64(ctx, platform, "FSBFrequency", ctx->fsb_frequency);

  /*
   * Retain the measured counter frequency and initial value for consumers
   * that support these properties; current XNU uses the bus property above.
   */
  dt_prop_u64(ctx, platform, "TSCFrequency", tsc_frequency);
  dt_prop_u64(ctx, platform, "InitialTSC", initial_tsc);
  log_info(L"TSC: initial=0x%lx estimated frequency=%lu Hz\r\n",
           initial_tsc, tsc_frequency);
  log_info(L"FSB: frequency=%lu Hz\r\n", ctx->fsb_frequency);

  /*
   * system-id: 16-byte hardware UUID. Boot.efi reads this from the DT
   * (Apple firmware pre-populates it) or from EFI variable "p".
   * Use a plausible fixed UUID for QEMU (Mac Pro 6,1 style).
   */
  {
    UINT8 uuid[16] = {
      0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x00, 0x01,
      0x80, 0x00, 0x00, 0x26, 0xB9, 0x58, 0x4C, 0x4A
    };
    dt_prop(ctx, platform, "system-id", uuid, 16);
  }

  /*
   * boot.efi iterates EFI_SYSTEM_TABLE.ConfigurationTable and creates one
   * child node per entry with "guid" (16 bytes) and "table" (8 bytes).
   * ACPI_20 and ACPI 1.0 entries also get an "alias" property.
   * XNU uses this to locate ACPI without scanning memory.
   */
  log_info(L"DT: config-table loop (%lu entries)\r\n",
           (UINT64)ctx->st->NumberOfTableEntries);
  DeviceTreeNode *cfg_tbl = dt_create_node(ctx);
  dt_prop_str(ctx, cfg_tbl, "name", "configuration-table");

  static const EFI_GUID acpi20_guid = ACPI_20_TABLE_GUID;
  static const EFI_GUID acpi_guid   = ACPI_TABLE_GUID;

  for (UINTN ci = 0; ci < ctx->st->NumberOfTableEntries; ci++) {
    EFI_CONFIGURATION_TABLE *entry = &ctx->st->ConfigurationTable[ci];

    DeviceTreeNode *child = dt_create_node(ctx);

    {
      CHAR8 gname[37];
      EFI_GUID *g = &entry->VendorGuid;
      UINT8 *b = (UINT8 *)g;
      /* Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
      UINTN gi = 0;
      /* Data1 (4 bytes, big-endian display) */
      for (int j = 3; j >= 0; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 5; j >= 4; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 7; j >= 6; j--) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 8; j <= 9; j++) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi++] = '-';
      for (int j = 10; j <= 15; j++) { gname[gi++] = "0123456789abcdef"[b[j]>>4]; gname[gi++] = "0123456789abcdef"[b[j]&0xf]; }
      gname[gi] = '\0';
      dt_prop_str(ctx, child, "name", gname);
    }

    dt_prop(ctx, child, "guid", &entry->VendorGuid, 16);
    UINT64 tbl_addr = (UINT64)(UINTN)entry->VendorTable;
    dt_prop(ctx, child, "table", &tbl_addr, 8);

    if (CompareMem(&entry->VendorGuid, &acpi20_guid, 16) == 0)
      dt_prop_str(ctx, child, "alias", "ACPI_20");
    else if (CompareMem(&entry->VendorGuid, &acpi_guid, 16) == 0)
      dt_prop_str(ctx, child, "alias", "ACPI");

    dt_add_child(ctx, cfg_tbl, child);
  }
  log_info(L"DT: config-table done\r\n");

  DeviceTreeNode *rt_svcs = dt_create_node(ctx);
  dt_prop_str(ctx, rt_svcs, "name", "runtime-services");

  /*
   * table: physical address of the EFI runtime services table copy in
   * conventional memory (boot.c copies RT to tbl_phys+0x200 before EBS).
   * We don't know tbl_phys here, so store the original RT physical address,
   * XNU reads this to set up runtime calls and will use the SVAM-adjusted VA.
   * After SVAM: virtual = phys & 0x3FFFFFFF for runtime-flagged pages.
   */
  {
    UINT64 rt_phys = rt_table_phys
        ? rt_table_phys
        : (UINT64)(UINTN)(ctx->st->RuntimeServices);
    dt_prop_u64(ctx, rt_svcs, "table", rt_phys);
  }

  DeviceTreeNode *kcompat = dt_create_node(ctx);
  dt_prop_str(ctx, kcompat, "name", "kernel-compatibility");
/* Describes the kernel, not the loader: an ia32 loader still boots x86_64. */
#if defined(CONFIG_x86_64)
  dt_prop_u32(ctx, kcompat, "x86_64", 1);
#elif defined(__aarch64__)
  /* a real arm64/BCM2837 boot needs its own tree (FDT-derived board info,
   * mailbox-queried memory layout, no ACPI at all). This just lets the
   * kcompat marker match the arch actually compiling. */
  dt_prop_u32(ctx, kcompat, "arm64", 1);
#elif defined(__riscv)
  dt_prop_u32(ctx, kcompat, "riscv64", 1);
#else
#error "devtree.c: unsupported architecture"
#endif

  DeviceTreeNode *efi = dt_create_node(ctx);
  dt_prop_str(ctx, efi, "name", "efi");

  /*
   * firmware-vendor: UTF-16 string from EFI System Table.
   * Length = (wcslen + 1) * 2 bytes (includes null terminator).
   */
  {
    CHAR16 *vendor = ctx->st->FirmwareVendor;
    UINTN wlen = dt_wcs_len(vendor);
    UINT32 blen = (UINT32)((wlen + 1) * 2);
    dt_prop(ctx, efi, "firmware-vendor", vendor, blen);
  }

  /* firmware-revision: 4-byte UINT32 from EFI System Table */
  dt_prop_u32(ctx, efi, "firmware-revision", ctx->st->FirmwareRevision);

  /* firmware-abi: "EFI64" for 64-bit mode (efiMode=64 in boot_args) */
  dt_prop_str(ctx, efi, "firmware-abi", "EFI64");

  dt_add_child(ctx, efi, platform);
  dt_add_child(ctx, efi, cfg_tbl);
  dt_add_child(ctx, efi, rt_svcs);
  dt_add_child(ctx, efi, kcompat);

#if defined(__aarch64__)
  DeviceTreeNode *cpus = dt_create_node(ctx);
  dt_prop_str(ctx, cpus, "name", "cpus");

  /*
   * Keep this node compatible with the ARM device-tree binding.  XNU's
   * topology parser currently accepts a 32-bit reg, but other consumers use
   * the parent cell sizes when decoding CPU IDs.
   */
  dt_prop_u32(ctx, cpus, "#address-cells", 2);
  dt_prop_u32(ctx, cpus, "#size-cells", 0);

  /* One node per enabled processor; xnu's virt board config caps MAX_CPUS at 8.
   * Only the boot CPU carries state "running" - that is how xnu picks it out. */
  {
    UINT64 mpidr[32];
    UINT32 cap[32] = { 0 }, cap_min = 0, cap_max = 0;
    UINTN ncpu = acpi_cpu_mpidrs(ctx, mpidr, 32);
    if (ncpu == 0)
      ncpu = fdt_cpu_mpidrs(ctx, mpidr, cap, 32);
    for (UINTN ci = 0; ci < ncpu; ci++) {
      if (cap[ci] != 0 && (cap_min == 0 || cap[ci] < cap_min))
        cap_min = cap[ci];
      if (cap[ci] > cap_max)
        cap_max = cap[ci];
    }
    UINT64 cntfrq;

    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    if (ncpu == 0) {
      mpidr[0] = 0;
      ncpu = 1;
    }
    for (UINTN ci = 0; ci < ncpu; ci++) {
      CHAR8 name[8] = { 'c', 'p', 'u', '@', 0, 0, 0, 0 };
      DeviceTreeNode *cpu = dt_create_node(ctx);

      if (ci >= 10) {
        name[4] = (CHAR8)('0' + (ci / 10));
        name[5] = (CHAR8)('0' + (ci % 10));
      } else {
        name[4] = (CHAR8)('0' + ci);
      }
      dt_prop_str(ctx, cpu, "name", name);
      dt_prop_str(ctx, cpu, "device_type", "cpu");
      dt_prop_str(ctx, cpu, "state", ci == 0 ? "running" : "waiting");
      dt_prop_u64(ctx, cpu, "reg", mpidr[ci]);
      dt_prop_u32(ctx, cpu, "timebase-frequency", (UINT32)cntfrq);
      // mixed core types (capacity-dmips-mhz differs): "P" for the fastest, "E" for the rest, as Apple's
      // device trees spell cluster-type. xnu then makes one cluster of each
      if (cap_min != 0 && cap_min != cap_max && cap[ci] != 0)
        dt_prop_str(ctx, cpu, "cluster-type", cap[ci] == cap_max ? "P" : "E");
      dt_add_child(ctx, cpus, cpu);
    }
    log_info(L"DT: published %lu cpu node(s)\r\n", (UINT64)ncpu);
  }

  DeviceTreeNode *armio = dt_create_node(ctx);
  dt_prop_str(ctx, armio, "name", "arm-io");
#if defined(XNU_LOADER_PLATFORM_GENERIC)
  /* Board drivers in the platform expert still key on this tag, so the boards they
   * know keep theirs; anything else read from its device tree is "fdt-io". */
  {
    CONST CHAR8 *io_type = "fdt-io";
    if (g_board.is_qemu_virt)
      io_type = "qemuvirt-io";
#if defined(XNU_LOADER_PLATFORM_SUN50I)
    io_type = "sun50i-io";
#elif defined(XNU_LOADER_PLATFORM_SG2002)
    io_type = "sg2002-io";
#endif
    dt_prop_str(ctx, armio, "device_type", io_type);
  }
  {
    /* Peripheral window. XNU's pe_arm_get_soc_base_phys() returns the second
     * cell, and the reg offsets below are relative to it. */
    UINT64 ranges[3] = { 0, g_board.soc_base, g_board.soc_size };
    dt_prop(ctx, armio, "ranges", ranges, sizeof(ranges));
  }
  if (g_board.uart_kind != FDT_UART_NONE) {
    /* xnu 12377's serial_init picks a driver by matching "compatible" on the
     * node named by /defaults serial-device, so the node needs both that
     * string and a phandle to be reachable at all. Every 16550 goes to
     * pe_serial.c's DesignWare driver, with its layout and clock spelled out. */
    DeviceTreeNode *uart0 = dt_create_node(ctx);
    UINT64 uart_reg[2] = { g_board.uart_base - g_board.soc_base, g_board.uart_size };
    dt_prop_str(ctx, uart0, "name", "uart0");
    dt_prop_u32(ctx, uart0, "AAPL,phandle", XNU_LOADER_UART0_PHANDLE);
    dt_prop(ctx, uart0, "reg", uart_reg, sizeof(uart_reg));
    if (g_board.uart_kind == FDT_UART_PL011) {
      dt_prop_str(ctx, uart0, "compatible", "arm,pl011");
    } else {
      dt_prop_str(ctx, uart0, "compatible", "snps,dw-apb-uart");
      dt_prop_u32(ctx, uart0, "reg-shift", g_board.uart_shift);
      dt_prop_u32(ctx, uart0, "reg-io-width", g_board.uart_width);
      if (g_board.uart_clock)
        dt_prop_u32(ctx, uart0, "clock-frequency", g_board.uart_clock);
      dt_prop_u32(ctx, uart0, "current-speed", g_board.uart_baud);
    }
    dt_add_child(ctx, armio, uart0);
  }
  if (g_board.gic_version) {
    /* gic-400: distributor then cpu interface. gic-v3: distributor then the
     * redistributor region, walked per cpu by GICR_TYPER. */
    DeviceTreeNode *gic = dt_create_node(ctx);
    UINT64 gic_reg[4] = { g_board.gicd_base - g_board.soc_base, g_board.gicd_size,
                          g_board.gic2_base - g_board.soc_base, g_board.gic2_size };
    dt_prop_str(ctx, gic, "name", "gic");
    dt_prop_str(ctx, gic, "compatible", g_board.gic_version == 3 ? "arm,gic-v3" : "arm,gic-400");
    dt_prop(ctx, gic, "reg", gic_reg, sizeof(gic_reg));
#if defined(XNU_LOADER_PLATFORM_SC8280XP)
    /* the controller for device interrupts (PDArmGICSPI) */
    dt_prop(ctx, gic, "interrupt-controller", NULL, 0);
    dt_prop_u32(ctx, gic, "#interrupt-cells", 1);
    dt_prop_u32(ctx, gic, "AAPL,phandle", SC8280XP_GIC_PHANDLE);
#endif
    dt_add_child(ctx, armio, gic);
  }
  log_info(L"DT: board uart 0x%lx gic%u 0x%lx/0x%lx arm-io 0x%lx+0x%lx%s\r\n",
           g_board.uart_base, g_board.gic_version, g_board.gicd_base, g_board.gic2_base,
           g_board.soc_base, g_board.soc_size, g_board.from_fdt ? L" (from fdt)" : L"");
#else
  dt_prop_str(ctx, armio, "device_type", "bcm2837-io");
  {
    UINT64 ranges[3] = { 0, 0x3F000000ULL, 0x01000000ULL };
    dt_prop(ctx, armio, "ranges", ranges, sizeof(ranges));
  }
#endif

#if defined(XNU_LOADER_PLATFORM_GENERIC)
  // only boards whose tree has a generic ecam host bridge, qemu virt's
  DeviceTreeNode *pci = g_board.has_pci_ecam ? dt_create_node(ctx) : NULL;
  if (pci) {
    UINT32 bus_range[2] = { 0, 255 };
    dt_prop_str(ctx, pci, "name", "pci");
    dt_prop_str(ctx, pci, "device_type", "pci");
    dt_prop_str(ctx, pci, "compatible", "pci-host-ecam-generic");
    dt_prop_u32(ctx, pci, "#address-cells", 3);
    dt_prop_u32(ctx, pci, "#size-cells", 2);
    dt_prop(ctx, pci, "bus-range", bus_range, sizeof(bus_range));
  }
#endif
#endif

  DeviceTreeNode *root = dt_create_node(ctx);
  dt_prop_str(ctx, root, "name", "device-tree");
#if defined(__riscv)
  // the board's own nodes, compatible and model come over from the fdt
  status = dt_import_fdt(ctx, root, chosen);
  if (EFI_ERROR(status)) {
    uefi_call_wrapper(ctx->bs->FreePages, 2, dt_addr, XNU_DEVTREE_PAGES);
    return status;
  }
#else
  /* IOKit matches the platform driver (AppleI386GenericPlatform) on these two
   * properties being present on the root node. Without them the entire IOKit
   * driver cascade fails to start and XNU hangs waiting for the root device. */
  dt_prop_str(ctx, root, "compatible", "ACPI");
  dt_prop_str(ctx, root, "model", "ACPI");
#if defined(__aarch64__) && defined(XNU_LOADER_PLATFORM_GENERIC)
  /* A real board's enabled devices come over from its tree, so the platform
   * expert can publish nubs for them. QEMU virt keeps the tree it always had. */
  if (g_board.from_fdt && !g_board.is_qemu_virt)
    dt_import_fdt(ctx, root, chosen);
#endif
#endif
  dt_add_child(ctx, root, chosen);

  /* XNU's IODTPlatformExpert requires the standard NVRAM options node. */
  {
    DeviceTreeNode *options = dt_create_node(ctx);
    dt_prop_str(ctx, options, "name", "options");

    {
      UINT8 pu[16];
      BOOLEAN have = smbios_system_uuid(ctx, pu);
      if (!have && boot_uuid_str[0]) have = uuid_str_to_bytes(boot_uuid_str, pu);
      if (have) dt_prop(ctx, options, "platform-uuid", pu, sizeof(pu));
    }

    dt_add_child(ctx, root, options);
  }

  /* macOS userland (kmutil) panics with "failed to get product node" without /product. */
  {
    DeviceTreeNode *product = dt_create_node(ctx);
    dt_prop_str(ctx, product, "name", "product");
    dt_prop_str(ctx, product, "product-name", "PureDarwin Virtual Machine");
    dt_add_child(ctx, root, product);
  }
#if defined(__aarch64__)
  dt_add_child(ctx, root, cpus);
  dt_add_child(ctx, root, armio);
#if defined(XNU_LOADER_PLATFORM_GENERIC)
  if (pci)
    dt_add_child(ctx, root, pci);
#if defined(XNU_LOADER_PLATFORM_SC8280XP)
  dt_add_child(ctx, root, sc8280xp_soc(ctx));
#endif
  if (g_board.psci_method) {
    // how to reach psci firmware; an hvc with no el2 underneath is an undefined instruction
    DeviceTreeNode *psci = dt_create_node(ctx);
    dt_prop_str(ctx, psci, "name", "psci");
    dt_prop_str(ctx, psci, "compatible", "arm,psci-1.0");
    dt_prop_str(ctx, psci, "method", g_board.psci_method == 2 ? "smc" : "hvc");
    dt_add_child(ctx, root, psci);
  }
#endif

  /*
   * /defaults: real iBoot always provides this node (even when empty).
   * pmap_bootstrap() -> pmap_compute_io_rgns() (osfmk/arm/pmap.c) does
   * SecureDTLookupEntry(NULL, "/defaults", &entry) unconditionally, guarded
   * only by assert() (compiled out in RELEASE, live in DEBUG/DEVELOPMENT).
   * The "pmap-io-ranges" property lookup under it is already handled
   * gracefully (falls back to "no I/O regions" if missing) - it's just the
   * node's *existence* that's required.
   */
  {
    DeviceTreeNode *defaults = dt_create_node(ctx);
    dt_prop_str(ctx, defaults, "name", "defaults");
#if defined(XNU_LOADER_PLATFORM_GENERIC)
    /* kern.hv_vmm_present reads this; without it userland believes it is on
     * bare metal and never takes its paravirtualised paths. A real board
     * described by its own tree is bare metal. */
    if (g_board.is_qemu_virt || !g_board.from_fdt)
      dt_prop_u32(ctx, defaults, "vmm-present", 1);
    /* Without this serial_init() returns early and the kernel is silent. */
    if (g_board.uart_kind != FDT_UART_NONE)
      dt_prop_u32(ctx, defaults, "serial-device", XNU_LOADER_UART0_PHANDLE);
#else
    dt_prop_u32(ctx, defaults, "vmm-present", 1);
#endif
    dt_add_child(ctx, root, defaults);
  }
#endif
#if defined(__riscv)
  {
    // pmap and the platform expert look /defaults up whether or not it has anything
    DeviceTreeNode *defaults = dt_create_node(ctx);
    dt_prop_str(ctx, defaults, "name", "defaults");
    if (dt_riscv_fdt_is_qemu(ctx))
      dt_prop_u32(ctx, defaults, "vmm-present", 1);
    dt_add_child(ctx, root, defaults);
  }
#endif
  dt_add_child(ctx, root, efi);

  log_info(L"DT: flattening into %lu-byte buffer\r\n", (UINT64)dt_size);
#if defined(__riscv) || defined(__aarch64__)
  // measured first, a converted fdt can be larger than the buffer
  if (dt_flat_size(root) > dt_size) {
    log_info(L"DT: tree needs %u bytes, more than %u\r\n", dt_flat_size(root), (UINT32)dt_size);
    uefi_call_wrapper(ctx->bs->FreePages, 2, dt_addr, XNU_DEVTREE_PAGES);
    return EFI_BUFFER_TOO_SMALL;
  }
#endif
  UINT32 used = dt_flatten_node(root, dt_base);
  if (used > dt_size) {
    log_info(L"DT: overflow %u > %u\n", used, (UINT32)dt_size);
    uefi_call_wrapper(ctx->bs->FreePages, 2, dt_addr, XNU_DEVTREE_PAGES);
    return EFI_BUFFER_TOO_SMALL;
  }

  log_info(L"DT: built %u bytes\n", used);

  *out_blob = (VOID *)(UINTN)dt_addr;
  *out_size = used;

  out_buf->ptr = (VOID *)(UINTN)dt_addr;
  out_buf->phys = dt_addr;
  out_buf->size = used;
  out_buf->pages = 2;

  return EFI_SUCCESS;
}
