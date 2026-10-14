#include "boot.h"
#include "console.h"
#include "devtree.h"
#include "fileio.h"
#include "serial.h"
#include <efiprot.h>
#if defined(LEGACY_BIOS)
#include "efi_emulation.h"
#endif

/* IRQ mask/unmask, real on both architectures (not stubs) - x86's
 * cli/sti and arm64's DAIF.I bit are each a single instruction. */
#if defined(PD_ARCH_X86)
#define IRQ_DISABLE() __asm__ volatile ("cli")
#define IRQ_ENABLE()  __asm__ volatile ("sti")
#elif defined(__aarch64__)
#define IRQ_DISABLE() __asm__ volatile ("msr daifset, #2")
#define IRQ_ENABLE()  __asm__ volatile ("msr daifclr, #2")
#elif defined(__riscv)
#define IRQ_DISABLE() __asm__ volatile ("csrci sstatus, 2")
#define IRQ_ENABLE()  __asm__ volatile ("csrsi sstatus, 2")
#else
#error "boot.c: unsupported architecture"
#endif

/* Attempts allowed before ExitBootServices is declared unworkable. Firmware
 * that is going to accept a key does so within the first couple of tries. */
#define EBS_MAX_ATTEMPTS 16

static UINT64 align_up_u64(UINT64 v, UINT64 a) {
  return (v + (a - 1)) & ~(a - 1);
}

/* Walk ACPI XSDT to find a table by 4-byte signature. Returns physical
 * address of the table header, or 0 if not found. */
static UINT64 acpi_find_table(AppContext *ctx, const CHAR8 *sig) {
  static const EFI_GUID acpi20 = ACPI_20_TABLE_GUID;

  /* Find RSDP from EFI configuration table */
  UINT64 rsdp_addr = 0;
  for (UINTN i = 0; i < ctx->st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx->st->ConfigurationTable[i];
    if (CompareMem(&e->VendorGuid, &acpi20, sizeof(EFI_GUID)) == 0) {
      rsdp_addr = (UINT64)(UINTN)e->VendorTable;
      break;
    }
  }
  if (!rsdp_addr) return 0;

  /* RSDP: +24 = XSDT physical address (8 bytes) */
  UINT64 xsdt_addr = *(UINT64 *)(UINTN)(rsdp_addr + 24);
  if (!xsdt_addr) return 0;

  /* XSDT header: +4 = length (4 bytes); entries start at +36, each 8 bytes */
  UINT32 xsdt_len = *(UINT32 *)(UINTN)(xsdt_addr + 4);
  UINTN  n_entries = (xsdt_len - 36) / 8;
  UINT64 *entries  = (UINT64 *)(UINTN)(xsdt_addr + 36);

  for (UINTN i = 0; i < n_entries; i++) {
    UINT64 tbl = entries[i];
    if (!tbl) continue;
    CHAR8 *s = (CHAR8 *)(UINTN)tbl;
    if (s[0] == sig[0] && s[1] == sig[1] && s[2] == sig[2] && s[3] == sig[3])
      return tbl;
  }
  return 0;
}

static UINTN boot_ascii_len(const CHAR8 *s) {
  UINTN n = 0;
  if (!s)
    return 0;
  while (s[n] != '\0')
    n++;
  return n;
}

EFI_STATUS boot_collect_memory_map(
    AppContext *ctx,
    BootArgsState *state)
{
  EFI_STATUS status;
  UINTN map_size;
  EFI_MEMORY_DESCRIPTOR *tmp_map;
  UINTN key;
  UINTN desc_size;
  UINT32 desc_ver;
  LowMemBuffer low_map;

  if (!ctx || !state)
    return EFI_INVALID_PARAMETER;

  state->memory_map = NULL;
  state->memory_map_size = 0;
  state->memory_map_key = 0;
  state->descriptor_size = 0;
  state->descriptor_version = 0;
  state->memory_map_buf.ptr = NULL;
  state->memory_map_buf.phys = 0;
  state->memory_map_buf.size = 0;
  state->memory_map_buf.pages = 0;

  map_size = 0;
  tmp_map = NULL;
  key = 0;
  desc_size = 0;
  desc_ver = 0;

  status = uefi_call_wrapper(
      ctx->bs->GetMemoryMap,
      5,
      &map_size,
      tmp_map,
      &key,
      &desc_size,
      &desc_ver);

  if (status != EFI_BUFFER_TOO_SMALL)
    return status;

  map_size += desc_size * 8;

  status = app_alloc_pool(ctx, map_size, (VOID **)&tmp_map);
  if (EFI_ERROR(status))
    return status;

  status = uefi_call_wrapper(
      ctx->bs->GetMemoryMap,
      5,
      &map_size,
      tmp_map,
      &key,
      &desc_size,
      &desc_ver);
  if (EFI_ERROR(status)) {
    app_free_pool(ctx, tmp_map);
    return status;
  }

  status = lowmem_realloc_copy(
      ctx,
      tmp_map,
      map_size,
      EfiLoaderData,
      &low_map);

  app_free_pool(ctx, tmp_map);

  if (EFI_ERROR(status))
    return status;

  state->memory_map = (EFI_MEMORY_DESCRIPTOR *)low_map.ptr;
  state->memory_map_size = map_size;
  state->memory_map_key = key;
  state->descriptor_size = desc_size;
  state->descriptor_version = desc_ver;
  state->memory_map_buf = low_map;

  return EFI_SUCCESS;
}

EFI_STATUS boot_refresh_memory_map(
    AppContext *ctx,
    BootArgsState *state)
{
  EFI_STATUS status;
  UINTN map_size;
  UINTN key;
  UINTN desc_size;
  UINT32 desc_ver;

  if (!ctx || !state)
    return EFI_INVALID_PARAMETER;

  for (;;) {
    map_size = state->memory_map_buf.size;
    key = 0;
    desc_size = 0;
    desc_ver = 0;

    if (state->memory_map_buf.ptr == NULL) {
      LowMemBuffer new_buf;
      status = lowmem_alloc_pages(ctx, EFI_PAGE_SIZE * 4, EfiLoaderData, &new_buf);
      if (EFI_ERROR(status))
        return status;
      state->memory_map_buf = new_buf;
      state->memory_map = (EFI_MEMORY_DESCRIPTOR *)new_buf.ptr;
      state->memory_map_size = 0;
    }

    map_size = state->memory_map_buf.pages << EFI_PAGE_SHIFT;

    status = uefi_call_wrapper(
        ctx->bs->GetMemoryMap,
        5,
        &map_size,
        state->memory_map_buf.ptr,
        &key,
        &desc_size,
        &desc_ver);

    if (status == EFI_BUFFER_TOO_SMALL) {
      LowMemBuffer old_buf = state->memory_map_buf;
      LowMemBuffer new_buf;
      UINTN needed = map_size + desc_size * 8;

      status = lowmem_alloc_pages(ctx, needed, EfiLoaderData, &new_buf);
      if (EFI_ERROR(status))
        return status;

      state->memory_map_buf = new_buf;
      state->memory_map = (EFI_MEMORY_DESCRIPTOR *)new_buf.ptr;

      if (old_buf.ptr != NULL)
        lowmem_free(ctx, &old_buf);

      continue;
    }

    if (EFI_ERROR(status))
      return status;

    state->memory_map = (EFI_MEMORY_DESCRIPTOR *)state->memory_map_buf.ptr;
    state->memory_map_size = map_size;
    state->memory_map_key = key;
    state->descriptor_size = desc_size;
    state->descriptor_version = desc_ver;
    return EFI_SUCCESS;
  }
}

VOID boot_update_args_memory_map(AppContext *ctx, BootArgsState *state) {
  if (!state || !state->args)
    return;

  state->args->MemoryMap = (UINT32)state->memory_map_buf.phys;
  state->args->MemoryMapSize = (UINT32)state->memory_map_size;
  state->args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  state->args->MemoryMapDescriptorVersion = state->descriptor_version;
  state->args->PhysicalMemorySize = app_detect_physical_memory_size(ctx);
}

EFI_STATUS boot_set_command_line(BootArgsState *state, boot_args *args, const CHAR8 *cmdline) {
  if (!state || !args || !cmdline)
    return EFI_INVALID_PARAMETER;

  SetMem(args->CommandLine, BOOT_LINE_LENGTH, 0);

  UINTN len = boot_ascii_len(cmdline);
  if (len >= BOOT_LINE_LENGTH)
    len = BOOT_LINE_LENGTH - 1;

  CopyMem(args->CommandLine, cmdline, len);
  args->CommandLine[len] = '\0';

  return EFI_SUCCESS;
}

/*
 * csr-active-config=0x... in the command line is what csrutil would have put
 * in NVRAM. x86 takes it through boot_args; arm64 reads /chosen/asmb lp-sip0
 * from the device tree. Without it every SIP check is enforced (config 0).
 */
BOOLEAN boot_cmdline_csr_config(const CHAR8 *cmdline, UINT32 *out) {
  static const CHAR8 key[] = "csr-active-config=";
  if (!cmdline || !out)
    return FALSE;
  for (UINTN i = 0; cmdline[i]; i++) {
    UINTN j = 0;
    const CHAR8 *p;
    UINT32 v = 0;
    if (i != 0 && cmdline[i - 1] != ' ')
      continue;
    while (key[j] && cmdline[i + j] == key[j])
      j++;
    if (key[j])
      continue;
    p = cmdline + i + j;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
      p += 2;
    for (; *p && *p != ' '; p++) {
      CHAR8 c = *p;
      UINT32 d = (c >= '0' && c <= '9') ? (UINT32)(c - '0') :
                 (c >= 'a' && c <= 'f') ? (UINT32)(c - 'a' + 10) :
                 (c >= 'A' && c <= 'F') ? (UINT32)(c - 'A' + 10) : 16;
      if (d == 16)
        break;
      v = (v << 4) | d;
    }
    *out = v;
    return TRUE;
  }
  return FALSE;
}

BOOLEAN boot_cmdline_has_flag(const CHAR8 *cmdline, const CHAR8 *flag) {
  if (!cmdline || !flag)
    return FALSE;

  UINTN flen = boot_ascii_len(flag);
  for (UINTN i = 0; cmdline[i]; i++) {
    if (i != 0 && cmdline[i - 1] != ' ')
      continue;

    UINTN j = 0;
    while (j < flen && cmdline[i + j] == flag[j])
      j++;
    if (j == flen && (cmdline[i + j] == '\0' || cmdline[i + j] == ' '))
      return TRUE;
  }
  return FALSE;
}

EFI_STATUS boot_build_args(
    AppContext *ctx,
    const CHAR8 *cmdline,
    MachoLoadResult *load_result,
    BootArgsState *state)
{
  EFI_STATUS status;
  LowMemBuffer args_buf;
  boot_args *args;

  if (!ctx || !load_result || !state || !state->memory_map)
    return EFI_INVALID_PARAMETER;

  if (state->args != NULL)
    return EFI_ALREADY_STARTED;

  {
    EFI_PHYSICAL_ADDRESS args_phys = XNU_BOOTARGS_PHYS;
    status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, 1, &args_phys);
    if (EFI_ERROR(status)) {
      log_error(L"boot_args: AllocateAddress(0x%lx) failed: %r\r\n",
                (UINT64)XNU_BOOTARGS_PHYS, status);
      return status;
    }
    args_buf.ptr = (VOID *)(UINTN)args_phys;
    args_buf.phys = args_phys;
    args_buf.size = sizeof(boot_args);
    args_buf.pages = 1;
  }

  args = (boot_args *)args_buf.ptr;
  SetMem(args, sizeof(boot_args), 0);

  state->args = args;
  state->args_buf = args_buf;

  status = boot_set_command_line(state, args, cmdline);
  if (EFI_ERROR(status))
    return status;

  /* Copy EFI tables to conventional memory before building the device tree
   * so state->rt_table_phys is valid when dt_build stores the runtime-services
   * table address in /efi/runtime-services/table.
   * OVMF places EFI_SYSTEM_TABLE and EFI_RUNTIME_SERVICES in runtime-services
   * pages (type 5/6) which XNU excludes from pmap_memory_regions. After
   * pmap_bootstrap the physmap doesn't cover those pages, so phys_to_kva()
   * produces KVAs that fault on access. Copy the structs to a pinned page in
   * conventional memory (always in the physmap) so XNU can read them. */
  {
    EFI_PHYSICAL_ADDRESS tbl_phys = XNU_EFITABLES_PHYS;
    if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, 1, &tbl_phys))) {
      UINT8 *page = (UINT8 *)(UINTN)tbl_phys;
      EFI_SYSTEM_TABLE *st_copy = (EFI_SYSTEM_TABLE *)page;
      EFI_RUNTIME_SERVICES *rt_copy = (EFI_RUNTIME_SERVICES *)(page + 0x200);
      state->rt_table_phys = (UINT64)(UINTN)rt_copy;

      XnuCopyMem(st_copy, ctx->st, sizeof(EFI_SYSTEM_TABLE));
      XnuCopyMem(rt_copy, ctx->st->RuntimeServices, sizeof(EFI_RUNTIME_SERVICES));
      /*
       * XNU's efi_set_tables_64() dereferences SystemTable->RuntimeServices
       * DIRECTLY (no ml_static_ptovirt), unlike the configuration-table walk.
       * So this pointer must already be the kernel physmap VA
       * (VM_MIN_KERNEL_ADDRESS | phys), not the raw physical address, or the
       * first access faults in the user VA range and crashes in vm_fault
       * (current_task() is still NULL this early).
       */
      st_copy->RuntimeServices =
          (EFI_RUNTIME_SERVICES *)(UINTN)(0xFFFFFF8000000000ULL |
                                          (UINT64)(UINTN)rt_copy);

      /* Also copy the EFI configuration table array to conventional memory
       * so XNU can walk it to find the ACPI RSDP.  The array pointer in the
       * original system table often points to EFI runtime/boot-services data
       * which is excluded from XNU's physmap after pmap_bootstrap.
       *
       * Layout: page+0x000=EFI_SYSTEM_TABLE, page+0x200=EFI_RUNTIME_SERVICES,
       *         page+0x400=EFI_CONFIGURATION_TABLE[n] */
      if (ctx->st->NumberOfTableEntries > 0 && ctx->st->ConfigurationTable) {
        EFI_CONFIGURATION_TABLE *cfg_copy =
            (EFI_CONFIGURATION_TABLE *)(page + 0x400);
        UINTN cfg_size =
            ctx->st->NumberOfTableEntries * sizeof(EFI_CONFIGURATION_TABLE);
        XnuCopyMem(cfg_copy, ctx->st->ConfigurationTable, cfg_size);
        st_copy->ConfigurationTable = cfg_copy;
        st_copy->NumberOfTableEntries = ctx->st->NumberOfTableEntries;
        log_info(L"EFI config table: %lu entries copied to 0x%lx\r\n",
                 ctx->st->NumberOfTableEntries, (UINT64)(UINTN)cfg_copy);

        /*
         * XNU's configuration-table walk (efi_get_cfgtbl_by_guid, reached from
         * efi_get_rsdp_physaddr) reads the *64-bit* EFI_SYSTEM_TABLE layout
         * unconditionally - it does not consult boot_args->efiMode the way
         * efi_init's efi_set_tables_32/64 split does.
         *
         * God, I hate Apple sometimes.
         */
        if (sizeof(VOID *) == 4) {
          UINT8  *st_bytes = (UINT8 *)st_copy;
          UINT8  *wide     = (UINT8 *)(page + 0x800);
          UINTN   entries  = ctx->st->NumberOfTableEntries;
          UINTN   i;

          /* One page is allocated for all of this, so the widened array has
           * 0x1000 - 0x800 bytes to live in. Firmware reports ~10 entries;
           * refuse rather than run off the page if that ever grows. */
          if (entries > (0x1000 - 0x800) / 24) {
            serial_trace((CONST CHAR8 *)"WARNING cfg entries exceed page, clamping ",
                         (UINT64)entries);
            entries = (0x1000 - 0x800) / 24;
          }

          /* Widen each entry to the 64-bit layout: GUID (16) + 8-byte
           * VendorTable, giving the 24-byte stride the walk steps by. */
          for (i = 0; i < entries; i++) {
            UINT8 *src = (UINT8 *)ctx->st->ConfigurationTable +
                         i * sizeof(EFI_CONFIGURATION_TABLE);
            UINT8 *dst = wide + i * 24;
            UINTN  b;
            UINT64 vendor_table;

            for (b = 0; b < 16; b++)      /* VendorGuid */
              dst[b] = src[b];

            vendor_table = (UINT64)(UINTN)((EFI_CONFIGURATION_TABLE *)src)->VendorTable;
            for (b = 0; b < 8; b++)
              dst[16 + b] = (UINT8)(vendor_table >> (8 * b));
          }

          /* The 64-bit aliases, written past the 32-bit struct's end. */
          {
            UINT64 wide_entries = (UINT64)entries;
            UINT64 wide_table   = (UINT64)(UINTN)wide;
            UINTN  b;

            for (b = 0; b < 8; b++) {
              st_bytes[0x68 + b] = (UINT8)(wide_entries >> (8 * b));
              st_bytes[0x70 + b] = (UINT8)(wide_table   >> (8 * b));
            }
          }

          /*
           * Complete the 64-bit presentation. XNU's 64-bit path reads only
           * Hdr, RuntimeServices (0x58), NumberOfTableEntries (0x68) and
           * ConfigurationTable (0x70), so aliasing those is enough to make the
           * table readable as 64-bit; the 32-bit fields below 0x48 stay put
           * and do not collide.
           */
          {
            UINT64 rt_va = 0xFFFFFF8000000000ULL | (UINT64)(UINTN)rt_copy;
            UINTN  b;

            for (b = 0; b < 8; b++)
              st_bytes[0x58 + b] = (UINT8)(rt_va >> (8 * b));

            /* Every consumer CRCs HeaderSize bytes, so it has to cover the
             * aliases now that they carry meaning. */
            st_copy->Hdr.HeaderSize = 0x78;
          }

          serial_trace((CONST CHAR8 *)"cfg64 table", (UINT64)(UINTN)wide);
          serial_trace((CONST CHAR8 *)"cfg64 count", (UINT64)entries);
          serial_trace((CONST CHAR8 *)"cfg64 rt   ", (UINT64)(UINTN)rt_copy);
        }
      }

      /* Recompute the EFI_SYSTEM_TABLE CRC32 after modifying RuntimeServices
       * and ConfigurationTable. XNU verifies CRC32 in both efi_set_tables_64
       * and efi_get_cfgtbl_by_guid; a stale checksum makes both return early,
       * breaking ACPI RSDP lookup and EFI runtime services setup. */
      {
        UINT32 new_crc = 0;
        st_copy->Hdr.CRC32 = 0;
        uefi_call_wrapper(ctx->bs->CalculateCrc32, 3,
                          st_copy, st_copy->Hdr.HeaderSize, &new_crc);
        st_copy->Hdr.CRC32 = new_crc;
        log_info(L"EFI_SYSTEM_TABLE CRC32 recomputed: 0x%x\r\n", new_crc);
      }

      args->efiSystemTable = (UINT32)(UINTN)tbl_phys;
      log_info(L"EFI tables copied to 0x%lx (st) / 0x%lx (rt)\r\n",
               (UINT64)tbl_phys, (UINT64)(tbl_phys + 0x200));
    } else {
      args->efiSystemTable = (UINT32)(UINTN)ctx->st;
      log_info(L"EFI table copy failed, using original 0x%lx\r\n",
               (UINT64)(UINTN)ctx->st);
    }

    /* Record physical page range for runtime-flagged descriptors.
     * Virtual page start is filled in after SetVirtualAddressMap in
     * exit_boot_services_retry once we know the actual virtual addresses. */
    {
      UINT8 *map = (UINT8 *)state->memory_map;
      UINT32 desc_sz = (UINT32)state->descriptor_size;
      UINT32 map_sz = (UINT32)state->memory_map_size;
      UINT32 min_pg = 0xFFFFFFFFU;
      UINT32 max_pg = 0;

      for (UINT32 off = 0; off + desc_sz <= map_sz; off += desc_sz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(map + off);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME))
          continue;
        UINT32 pg_start = (UINT32)(d->PhysicalStart >> EFI_PAGE_SHIFT);
        UINT32 pg_end   = pg_start + (UINT32)d->NumberOfPages;
        if (pg_start < min_pg) min_pg = pg_start;
        if (pg_end   > max_pg) max_pg = pg_end;
      }

      if (min_pg < max_pg) {
        args->efiRuntimeServicesPageStart = min_pg;
        args->efiRuntimeServicesPageCount = max_pg - min_pg;
        /* efiRuntimeServicesVirtualPageStart is set after SVAM. */
      }
    }

    args->csrActiveConfig = 0;
  }

  if (state->device_tree == NULL) {
    status = dt_build(
        ctx,
        &state->device_tree,
        &state->device_tree_size,
        &state->device_tree_buf,
        cmdline,
        state->rt_table_phys);
    if (EFI_ERROR(status)) {
      lowmem_free(ctx, &args_buf);
      return status;
    }
  }

  args->Revision = kBootArgsRevision1;
  args->Version  = kBootArgsVersion;

  /*
   * This is 64 even on 32-bit platforms.
   *
   * The reason behind this is because 32-bit EFI in XNU has rotted heavily,
   * and if PureDarwin ever decides to implement decompression in xnu-loader to
   * boot Apple kernels, this allows for that.
   */
  args->efiMode = kBootArgsEfiMode64;
  args->debugMode = 0;
  args->flags = kBootArgsFlagBlackBg | kBootArgsFlagLoginUI;
  {
    UINT32 csr;
    if (boot_cmdline_csr_config(cmdline, &csr)) {
      args->csrActiveConfig = csr;
      args->flags |= kBootArgsFlagCSRActiveConfig;
    }
  }

  args->MemoryMap = (UINT32)(UINTN)state->memory_map;
  args->MemoryMapSize = (UINT32)state->memory_map_size;
  args->MemoryMapDescriptorSize = (UINT32)state->descriptor_size;
  args->MemoryMapDescriptorVersion = state->descriptor_version;
  args->PhysicalMemorySize = app_detect_physical_memory_size(ctx);

  args->deviceTreeP = (UINT32)(UINTN)state->device_tree;
  args->deviceTreeLength = state->device_tree_size;

  args->kaddr = (UINT32)load_result->host_base;
  args->ksize = (UINT32)align_up_u64(load_result->image_size, EFI_PAGE_SIZE);

  args->KC_hdrs_vaddr = 0;
  args->kslide = ctx->kslide;

  args->FSBFrequency = ctx->fsb_frequency;

  /* pciConfigSpaceBaseAddress / StartBus / EndBus: from ACPI MCFG.
   * boot.efi reads MCFG[0] allocation structure at offset +44. */
  {
    UINT64 mcfg = acpi_find_table(ctx, "MCFG");
    if (mcfg) {
      /* MCFG allocation structure starts at offset 44 (36 header + 8 reserved).
       * BaseAddress[8] at +44, SegmentGroup[2] at +52, StartBus[1] at +54,
       * EndBus[1] at +55. */
      args->pciConfigSpaceBaseAddress   = *(UINT64 *)(UINTN)(mcfg + 44);
      args->pciConfigSpaceStartBusNumber = *(UINT8  *)(UINTN)(mcfg + 54);
      args->pciConfigSpaceEndBusNumber   = *(UINT8  *)(UINTN)(mcfg + 55);
      log_info(L"MCFG: PCI base=0x%lx bus=%u-%u\r\n",
               args->pciConfigSpaceBaseAddress,
               args->pciConfigSpaceStartBusNumber,
               args->pciConfigSpaceEndBusNumber);
    } else {
      log_info(L"MCFG: not found in ACPI\r\n");
    }
  }

  status = boot_fill_video(ctx, args);
  if (EFI_ERROR(status)) {
    lowmem_free(ctx, &args_buf);
    return status;
  }
  // the logo goes up as soon as the mode is known, the loader keeps working behind it
  boot_draw_logo(args);

  state->args = args;
  state->args_buf = args_buf;

#ifdef VERBOSE_BOOT
  log_info(L"sizeof(boot_args) = 0x%lx\r\n", (UINT64)sizeof(boot_args));
  log_info(L"boot_args low phys = 0x%lx\r\n", (UINT64)state->args_buf.phys);
  log_info(L"device_tree low phys = 0x%lx\r\n", (UINT64)state->device_tree_buf.phys);
  log_info(L"memory_map ptr  = 0x%lx\r\n", (UINT64)(UINTN)state->memory_map);
  log_info(L"memory_map phys = 0x%lx\r\n", (UINT64)state->memory_map_buf.phys);
#endif // VERBOSE_BOOT

  return EFI_SUCCESS;
}

/*
 * Locate the firmware's GOP and settle on a linear mode, reporting the result
 * in width/height/stride/base. found is left FALSE for every "no display here"
 * outcome - no GOP at all, or only Blt-only modes - which is not an error: XNU
 * boots fine on the serial console with v_display zeroed.
 *
 * Split out from boot_fill_video() because arm64's Boot_Video is the same six
 * fields at 64 bits wide, so only the probe can be shared, not the assignment.
 */
EFI_STATUS boot_probe_video(
    AppContext *ctx,
    const CHAR8 *cmdline,
    boot_video_info *out)
{
  if (!ctx || !out)
      return EFI_INVALID_PARAMETER;

  SetMem(out, sizeof(*out), 0);

  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

  EFI_STATUS status = uefi_call_wrapper(
      ctx->bs->LocateProtocol,
      3,
      &gEfiGraphicsOutputProtocolGuid,
      NULL,
      (VOID **)&gop);

  /* No GOP (headless firmware, no display attached, UGA-only board, etc.):
   * leave args->Video/VideoV1 zeroed (v_display=0) and continue - XNU boots
   * fine on the serial console alone (serial=3 in the boot-args cmdline)
   * without a framebuffer. Only actual protocol/mode errors after a GOP was
   * located are treated as fatal, since those indicate a broken GOP rather
   * than its absence. */
  if (EFI_ERROR(status) || !gop || !gop->Mode || !gop->Mode->Info) {
    log_info(L"boot_probe_video: no usable GOP (%r), continuing headless\r\n", status);
    return EFI_SUCCESS;
  }

  /* XNU needs a LINEAR framebuffer.  The firmware's current GOP mode is
   * normally the native panel resolution with a linear framebuffer, so keep it
   * when it is RGBX(0)/BGRX(1).  Only if the current mode is BltOnly/bitmask
   * (no CPU-addressable framebuffer) do we search for the highest-resolution
   * linear mode and switch to it. */
  {
    EFI_GRAPHICS_PIXEL_FORMAT curfmt = gop->Mode->Info->PixelFormat;
    BOOLEAN cur_linear =
        (curfmt == PixelRedGreenBlueReserved8BitPerColor ||
         curfmt == PixelBlueGreenRedReserved8BitPerColor);

    if (!cur_linear) {
      UINT32 best_mode = gop->Mode->MaxMode; /* sentinel = none found */
      UINT64 best_px   = 0;
      for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
        UINTN misz = 0;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &misz, &mi)) || !mi)
          continue;
        if (mi->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
            mi->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
          uefi_call_wrapper(ctx->bs->FreePool, 1, mi);
          continue;
        }
        UINT64 px = (UINT64)mi->HorizontalResolution * mi->VerticalResolution;
        if (px > best_px) {
          best_px   = px;
          best_mode = m;
        }
        uefi_call_wrapper(ctx->bs->FreePool, 1, mi);
      }
      if (best_mode == gop->Mode->MaxMode) {
        log_info(L"boot_probe_video: current mode not linear and no linear mode "
                 L"found, continuing headless\r\n");
        return EFI_SUCCESS;
      }
      log_info(L"boot_probe_video: current mode not linear, switching to mode %u\r\n",
               best_mode);
      status = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
      if (EFI_ERROR(status)) {
        log_info(L"boot_probe_video: SetMode(%u) failed: %r, continuing headless\r\n",
                 best_mode, status);
        return EFI_SUCCESS;
      }
    }
  }

  UINT32 width = gop->Mode->Info->HorizontalResolution;
  UINT32 height = gop->Mode->Info->VerticalResolution;
  UINT64 stride = (UINT64)gop->Mode->Info->PixelsPerScanLine * 4;
  UINT64 fb_base  = gop->Mode->FrameBufferBase;

  UINT8 pixel_fmt = (UINT8)gop->Mode->Info->PixelFormat;
  UINT64 aperture = gop->Mode->FrameBufferSize;
  /* Both public boot_args layouts represent stride as 32 bits. Do not publish
   * wrapped geometry or pixels extending beyond the actual GOP aperture. */
  if (!width || !height || !fb_base || !aperture || width > UINT32_MAX / 4 ||
      stride < (UINT64)width * 4 || stride > UINT32_MAX ||
      (UINT64)height > aperture / stride || fb_base > UINT64_MAX - aperture ||
      (pixel_fmt != PixelRedGreenBlueReserved8BitPerColor &&
       pixel_fmt != PixelBlueGreenRedReserved8BitPerColor)) {
    log_error(L"boot_probe_video: invalid firmware scanout geometry\r\n");
    return EFI_COMPROMISED_DATA;
  }

#ifdef VERBOSE_BOOT
  log_info(L"boot_probe_video: %ux%u stride=%u fb=0x%lx pixfmt=%u\r\n",
           width, height, (UINT32)stride, fb_base, (UINT32)pixel_fmt);
#endif // VERBOSE_BOOT

  out->found     = TRUE;
  out->base_addr = fb_base;
  out->row_bytes = stride;
  out->width     = width;
  out->height    = height;
  out->depth     = 32;
  out->aperture_length = aperture;
  out->pixel_format = pixel_fmt == PixelRedGreenBlueReserved8BitPerColor
                         ? NATIVE_BOOT_VIDEO_RGBX : NATIVE_BOOT_VIDEO_BGRX;
  out->display   = boot_cmdline_has_flag(cmdline, (const CHAR8 *)"-v")
                       ? FB_TEXT_MODE
                       : GRAPHICS_MODE;

  return EFI_SUCCESS;
}

EFI_STATUS boot_fill_video(
    AppContext *ctx,
    boot_args *args)
{
  boot_video_info vi;
  EFI_STATUS status;

  if (!ctx || !args)
      return EFI_INVALID_PARAMETER;

  status = boot_probe_video(ctx, args->CommandLine, &vi);
  if (EFI_ERROR(status) || !vi.found)
    return status;

  args->Video.v_display  = (UINT32)vi.display;
  args->Video.v_rowBytes = (UINT32)vi.row_bytes;
  args->Video.v_width    = (UINT32)vi.width;
  args->Video.v_height   = (UINT32)vi.height;
  args->Video.v_depth    = (UINT32)vi.depth;
  args->Video.v_rotate   = 0;
  /* ARM reaches PE_create_console() after kernel_map exists. XNU therefore
   * interprets an untagged v_baseAddr as an already-mapped kernel VA; bit 0
   * is its explicit "physical address, map this" tag (initialize_screen()
   * masks the low two bits before ml_io_map_unmappable()). */
  args->Video.v_baseAddr = vi.base_addr | 1ULL;

  /* VideoV1 (the struct XNU reads at boot_args+1048): base addr is 32-bit.
   * XNU's Boot_Video.v_baseAddr is 32-bit, so a framebuffer above 4GB cannot
   * be described, warn (some discrete GPUs place the FB high, but Macs and
   * most laptop iGPUs keep it below 4GB). */
  if (vi.base_addr > 0xFFFFFFFFULL)
    log_error(L"boot_fill_video: WARNING framebuffer 0x%lx > 4GB, truncated\r\n",
              vi.base_addr);
  args->VideoV1.v_baseAddr = (UINT32)vi.base_addr;
  args->VideoV1.v_display  = (UINT32)vi.display;
  args->VideoV1.v_rowBytes = (UINT32)vi.row_bytes;
  args->VideoV1.v_width    = (UINT32)vi.width;
  args->VideoV1.v_height   = (UINT32)vi.height;
  args->VideoV1.v_depth    = (UINT32)vi.depth;

  return EFI_SUCCESS;
}

#if defined(__aarch64__) || defined(__riscv)
/*
 * arm64's Boot_Video is 64-bit throughout and has no VideoV1 companion, so it
 * can carry a framebuffer anywhere in the address space without the 4GB
 * truncation the x86 struct suffers.
 */
EFI_STATUS arm64_boot_fill_video(
    AppContext *ctx,
    arm64_boot_args *args)
{
  boot_video_info vi;
  EFI_STATUS status;

  if (!ctx || !args)
      return EFI_INVALID_PARAMETER;

  status = boot_probe_video(ctx, (const CHAR8 *)args->CommandLine, &vi);
  if (EFI_ERROR(status) || !vi.found)
    return status;

  args->Video.v_baseAddr = vi.base_addr;
  /* ARM's PE_create_console() treats zero as "no graphics console" and any
   * nonzero value as a framebuffer-backed console. Preserve FB_TEXT_MODE here
   * just as the x86 handoff does; translating it to zero made verbose ARM
   * boots discard an otherwise valid GOP framebuffer. */
  args->Video.v_display  = vi.display;
  args->Video.v_rowBytes = vi.row_bytes;
  args->Video.v_width    = vi.width;
  args->Video.v_height   = vi.height;
  args->Video.v_depth    = vi.depth;

  return EFI_SUCCESS;
}
#endif /* __aarch64__ */

VOID boot_free_args(
    AppContext *ctx,
    BootArgsState *state)
{
  if (!ctx || !state)
    return;

  if (state->device_tree_buf.ptr != NULL)
    lowmem_free(ctx, &state->device_tree_buf);

  state->device_tree = NULL;
  state->device_tree_size = 0;

  if (state->args_buf.ptr != NULL)
    lowmem_free(ctx, &state->args_buf);

  state->args = NULL;

  if (state->memory_map_buf.ptr != NULL)
    lowmem_free(ctx, &state->memory_map_buf);

  state->memory_map = NULL;
  state->memory_map_size = 0;
  state->memory_map_key = 0;
  state->descriptor_size = 0;
  state->descriptor_version = 0;
}

VOID boot_log_args(BootArgsState *state) {
  if (!state || !state->args)
    return;

  log_info(L"boot_args @ 0x%lx\r\n", (UINT64)(UINTN)state->args);
  log_info(L"  Revision: %u\r\n", state->args->Revision);
  log_info(L"  Version: %u\r\n", state->args->Version);
  log_info(L"  deviceTreeP: 0x%x\r\n", state->args->deviceTreeP);
  log_info(L"  deviceTreeLength: 0x%x\r\n", state->args->deviceTreeLength);
  log_info(L"  Video base: 0x%x\r\n", state->args->Video.v_baseAddr);
  log_info(L"  Video %ux%u depth=%u rowBytes=%u\r\n",
      state->args->Video.v_width,
      state->args->Video.v_height,
      state->args->Video.v_depth,
      state->args->Video.v_rowBytes);
  log_info(L"  CommandLine: %a\r\n", state->args->CommandLine);
  log_info(L"  MemoryMap: 0x%x\r\n", state->args->MemoryMap);
  log_info(L"  MemoryMapSize: 0x%x\r\n", state->args->MemoryMapSize);
  log_info(L"  DescriptorSize: 0x%x\r\n", state->args->MemoryMapDescriptorSize);
  log_info(L"  kaddr: 0x%x\r\n", state->args->kaddr);
  log_info(L"  ksize: 0x%x\r\n", state->args->ksize);
  log_info(L"  efiSystemTable: 0x%x\r\n", state->args->efiSystemTable);
  log_info(L"  efiMode: %u\r\n", state->args->efiMode);
}


/*
 * Sort the EFI memory map ascending by PhysicalStart, in place.
 *
 * boot.efi calls SortMemoryMap around SetVirtualAddressMap; an ordered map is
 * what XNU's efi_init walks and lets us assign a single monotonically-increasing
 * packed virtual range.  Runs AFTER ExitBootServices, so it must not use any
 * BootServices call, CopyMem here is the loader's own XnuCopyMem.  descriptor
 * records are ~48 bytes; guard against anything larger than the temp buffer.
 */
static VOID boot_sort_memory_map(UINT8 *map, UINTN map_sz, UINTN desc_sz) {
  if (!map || desc_sz == 0)
    return;
  UINTN n = map_sz / desc_sz;
  UINT8 tmp[512];
  if (desc_sz > sizeof(tmp))
    return; /* would overflow tmp; leave unsorted rather than corrupt */

  for (UINTN i = 1; i < n; i++) {
    CopyMem(tmp, map + i * desc_sz, desc_sz);
    UINT64 key = ((EFI_MEMORY_DESCRIPTOR *)tmp)->PhysicalStart;
    INTN j = (INTN)i - 1;
    while (j >= 0 &&
           ((EFI_MEMORY_DESCRIPTOR *)(map + (UINTN)j * desc_sz))->PhysicalStart > key) {
      CopyMem(map + ((UINTN)j + 1) * desc_sz, map + (UINTN)j * desc_sz, desc_sz);
      j--;
    }
    CopyMem(map + ((UINTN)j + 1) * desc_sz, tmp, desc_sz);
  }
}

EFI_STATUS exit_boot_services_retry(
    AppContext *ctx,
    EFI_HANDLE image,
    BootArgsState *state)
{
  EFI_STATUS status;

  status = boot_refresh_memory_map(ctx, state);
  if (EFI_ERROR(status))
    return status;

  boot_update_args_memory_map(ctx, state);

  /*
   * Bounded so a firmware that never validates our key fails loudly instead of
   * spinning here forever with no output, which is indistinguishable from a
   * dead machine.
   */
  UINTN attempt = 0;
  for (attempt = 0; attempt < EBS_MAX_ATTEMPTS; attempt++) {
    UINTN map_size = state->memory_map_buf.pages << EFI_PAGE_SHIFT;
    UINTN key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;
    BOOLEAN mask_irqs = (attempt > 0);

    serial_puts8((CONST CHAR8 *)"[EBS] attempt ");
    serial_puthex((UINT64)attempt);
    serial_puts8(mask_irqs ? (CONST CHAR8 *)" (irqs masked)\r\n"
                           : (CONST CHAR8 *)" (irqs enabled)\r\n");

    if (mask_irqs)
      IRQ_DISABLE();

    status = uefi_call_wrapper(
        ctx->bs->GetMemoryMap,
        5,
        &map_size,
        state->memory_map_buf.ptr,
        &key,
        &desc_size,
        &desc_ver);

    if (EFI_ERROR(status)) {
      if (mask_irqs)
        IRQ_ENABLE();
      serial_trace((CONST CHAR8 *)"GetMemoryMap failed, status ", (UINT64)status);
      serial_trace((CONST CHAR8 *)"  need bytes ", (UINT64)map_size);
      serial_trace((CONST CHAR8 *)"  have bytes ",
                   (UINT64)(state->memory_map_buf.pages << EFI_PAGE_SHIFT));
      return status;
    }

    /* Update args while interrupts are still off - no UEFI calls. */
    state->memory_map_key = key;
    state->memory_map_size = map_size;
    state->descriptor_size = desc_size;
    state->descriptor_version = desc_ver;
    state->args->MemoryMap = (UINT32)state->memory_map_buf.phys;
    state->args->MemoryMapSize = (UINT32)map_size;
    state->args->MemoryMapDescriptorSize = (UINT32)desc_size;
    state->args->MemoryMapDescriptorVersion = desc_ver;

    status = uefi_call_wrapper(
        ctx->bs->ExitBootServices,
        2,
        image,
        key);

    if (status == EFI_SUCCESS) {
      serial_reinit();
      serial_mark((CONST CHAR8 *)"ExitBootServices returned SUCCESS");
      #if defined(XNU_LOADER_PLATFORM_SC8280XP)
        /* arm64 XNU never calls EFI runtime services, so the packing and 
        * SetVirtualAddressMap below serve nothing here. On the X13s the handoff
        * stalled in them. Firmware keeps its physical mapping. */
        return EFI_SUCCESS;
      #endif
      EFI_RUNTIME_SERVICES *rt = ctx->st->RuntimeServices;
      UINT8 *rmap = (UINT8 *)state->memory_map_buf.ptr;
      UINTN rdesc_sz = state->descriptor_size;
      UINTN rmap_sz  = state->memory_map_size;

      boot_sort_memory_map(rmap, rmap_sz, rdesc_sz);

      /*
       * Pack base = 2MB-aligned first VA above the kernel image, then EXTEND
       * ksize so XNU's physfree (= kaddr + ksize) covers the packed range.
       * XNU only keeps bootstrap page tables for [0, physfree), Idle_PTs_release
       * frees the rest before efi_init runs, so runtime VAs MUST live below
       * physfree.  By growing ksize we make the runtime region "part of the
       * kernel": its PT pages survive, its PTEs are identity-filled by pstart,
       * and efi_init remaps them to the real runtime physical pages.  This is
       * boot.efi's AllocKernelMem-region trick without physically moving pages.
       */
      /*
       * IMPORTANT: state->args->kaddr here is the STAGING host_base (high RAM,
       * ~0x7A8B4000) where segments were parked, NOT where XNU runs.  main.c
       * copies the image to the SLID base 0x100000 + kslide AFTER EBS and sets
       * args->kaddr to that.  XNU derives physfree and its bootstrap page
       * tables from that slid base, so the runtime VA window MUST be computed
       * relative to it, otherwise pmap_map_bd sees VAs with no page table.
       */
      UINT64 kaddr    = 0x100000ULL + ctx->kslide;
      UINT64 va_base  = XNU_RT_VA_BASE;

      UINT64 va_cursor   = va_base;
      UINT64 rmin_virt_pg = ~0ULL;
      for (UINTN roff = 0; roff + rdesc_sz <= rmap_sz; roff += rdesc_sz) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(rmap + roff);
        if (!(d->Attribute & EFI_MEMORY_RUNTIME))
          continue;
        UINT64 sz = (UINT64)d->NumberOfPages << EFI_PAGE_SHIFT;
#if defined(PD_ARCH_X86)
        // the firmware's own pointers must be kernel addresses: efi_init maps the window
        // there, and the low alias is user space whenever a task's pmap is loaded
        d->VirtualStart = 0xFFFFFF8000000000ULL | va_cursor;
#else
        d->VirtualStart = va_cursor;
#endif
        va_cursor += sz;

        UINT64 vpg = d->VirtualStart >> EFI_PAGE_SHIFT;
        if (vpg < rmin_virt_pg) rmin_virt_pg = vpg;
      }

      /* Grow ksize so physfree covers the packed runtime region (2MB-rounded). */
      UINT64 new_kend = (va_cursor + 0x1FFFFFULL) & ~0x1FFFFFULL;
      state->args->ksize = (UINT32)(new_kend - kaddr);

      serial_trace((CONST CHAR8 *)"SVAM va_base   ", va_base);
      serial_trace((CONST CHAR8 *)"SVAM va_cursor ", va_cursor);
      serial_trace((CONST CHAR8 *)"SVAM new_kend  ", new_kend);
      serial_trace((CONST CHAR8 *)"SVAM ksize     ", (UINT64)state->args->ksize);
      if (va_cursor > 0x3E800000ULL) {
        serial_trace((CONST CHAR8 *)"FATAL runtime VA window passes NKPT edge 0x3E800000, cursor ",
                     va_cursor);
        for (;;) { }
      }
      serial_mark((CONST CHAR8 *)"calling SetVirtualAddressMap");

      // through the wrapper like every other firmware call: a direct call passes the sysv registers
      // and the firmware rejects what it finds in the ms ones
      EFI_STATUS svam = uefi_call_wrapper(rt->SetVirtualAddressMap, 4, rmap_sz, rdesc_sz,
                               state->descriptor_version,
                               (EFI_MEMORY_DESCRIPTOR *)rmap);

#if defined(LEGACY_BIOS)
      /* This table was pinned before SVAM, so update its function pointers to
       * the virtual mapping selected for the shim's runtime image. */
      legacy_runtime_fixup(
          (EFI_RUNTIME_SERVICES *)(UINTN)state->rt_table_phys);
#elif defined(PD_ARCH_X86)
      // SVAM converted the firmware's own table (and its CRC): the pinned copy XNU calls
      // through still holds the physical entry points
      if (state->rt_table_phys)
        XnuCopyMem((VOID *)(UINTN)state->rt_table_phys, rt, sizeof(EFI_RUNTIME_SERVICES));
#endif

      serial_reinit();
      serial_trace((CONST CHAR8 *)"SetVirtualAddressMap returned ", (UINT64)svam);

      /* Update boot_args: virtual page start only.
       * efiSystemTable was already set to the conventional-memory copy
       * (tbl_phys) before EBS; that address is in XNU's physmap and
       * needs no adjustment after SVAM. */
      if (state->args) {
        if (rmin_virt_pg != ~0ULL)
          state->args->efiRuntimeServicesVirtualPageStart = rmin_virt_pg;
      }

      return EFI_SUCCESS;
    }

    if (mask_irqs)
      IRQ_ENABLE();

    /* Reporting happens here, with interrupts back on and the window closed,
     * so the diagnostics cannot themselves widen the race being diagnosed. */
    serial_trace((CONST CHAR8 *)"ExitBootServices rejected, status ",
                 (UINT64)status);
    serial_trace((CONST CHAR8 *)"  key       ", (UINT64)key);
    serial_trace((CONST CHAR8 *)"  map bytes ", (UINT64)map_size);
    serial_trace((CONST CHAR8 *)"  desc size ", (UINT64)desc_size);

    if (status != EFI_INVALID_PARAMETER)
      return status;
  }

  serial_trace((CONST CHAR8 *)"FATAL ExitBootServices never accepted our key after attempts ",
               (UINT64)EBS_MAX_ATTEMPTS);
  return EFI_INVALID_PARAMETER;
}

#if defined(__aarch64__) || defined(__riscv)
EFI_STATUS arm64_boot_build_args(
    AppContext *ctx,
    const CHAR8 *cmdline,
    UINT64 virt_base,
    UINT64 phys_base,
    UINT64 mem_size,
    UINT64 top_of_kernel_data,
    UINT64 device_tree_phys,
    UINT32 device_tree_len,
    LowMemBuffer *out_args_buf,
    arm64_boot_args **out_args)
{
  EFI_STATUS status;
  LowMemBuffer args_buf;
  arm64_boot_args *args;

  if (!ctx || !out_args_buf || !out_args)
    return EFI_INVALID_PARAMETER;

  {
    EFI_PHYSICAL_ADDRESS args_phys = XNU_ARM64_BOOTARGS_PHYS;
    status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
        AllocateAddress, EfiLoaderData, 1, &args_phys);
    if (EFI_ERROR(status)) {
      log_error(L"arm64 boot_args: AllocateAddress(0x%lx) failed: %r\r\n",
                (UINT64)XNU_ARM64_BOOTARGS_PHYS, status);
      return status;
    }
    args_buf.ptr = (VOID *)(UINTN)args_phys;
    args_buf.phys = args_phys;
    args_buf.size = sizeof(arm64_boot_args);
    args_buf.pages = 1;
  }

  args = (arm64_boot_args *)args_buf.ptr;
  SetMem(args, sizeof(arm64_boot_args), 0);

  args->Revision = ARM64_BOOT_ARGS_REVISION;
  args->Version = ARM64_BOOT_ARGS_VERSION;
  args->virtBase = virt_base;
  args->physBase = phys_base;
  args->memSize = mem_size;
  args->topOfKernelData = top_of_kernel_data;
  args->machineType = 0; /* unused by this kernel's pe_arm_init path */
  /* Virtual, not physical: pexpert/arm/pe_init.c hands deviceTreeP straight to
   * SecureDTInit with no ptov, so iBoot's contract is a kernel VA. The DT sits
   * below topOfKernelData, so arm_vm_init has it mapped at virtBase + delta. */
  args->deviceTreeP = virt_base + (device_tree_phys - phys_base);
  args->deviceTreeLength = device_tree_len;
  args->bootFlags = 0;
  args->memSizeActual = 0; /* 0 == "same as memSize", per real XNU's convention */

  if (cmdline) {
    UINTN len = 0;
    while (cmdline[len] && len < BOOT_LINE_LENGTH - 1) len++;
    CopyMem(args->CommandLine, cmdline, len);
    args->CommandLine[len] = '\0';
  }

  /* After CommandLine: the probe reads -v from it to pick text vs graphics
   * mode. Leaving Video zeroed (v_display = 0) is what happens when there is
   * no linear framebuffer, and is how this booted before - serial only. */
  (VOID)arm64_boot_fill_video(ctx, args);

  *out_args_buf = args_buf;
  *out_args = args;
  return EFI_SUCCESS;
}

VOID arm64_boot_log_args(arm64_boot_args *args)
{
  if (!args)
    return;

  log_info(L"arm64 boot_args @ 0x%lx\r\n", (UINT64)(UINTN)args);
  log_info(L"  Revision: %d  Version: %d\r\n", args->Revision, args->Version);
  log_info(L"  virtBase: 0x%lx  physBase: 0x%lx\r\n", args->virtBase, args->physBase);
  log_info(L"  memSize: 0x%lx  topOfKernelData: 0x%lx\r\n", args->memSize, args->topOfKernelData);
  log_info(L"  deviceTreeP: 0x%lx  deviceTreeLength: 0x%x\r\n",
           args->deviceTreeP, args->deviceTreeLength);
  log_info(L"  CommandLine: %a\r\n", args->CommandLine);
}
#endif /* __aarch64__ */
