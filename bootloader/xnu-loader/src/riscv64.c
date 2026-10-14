// the loader's efi application for riscv64: reads the kernel collection, places it
// behind a 1GB aligned virtual to physical delta and enters _start the way start.s expects
#include "common.h"
#include "app.h"
#include "boot.h"
#include "console.h"
#include "devtree.h"
#include "fileio.h"
#include "jump.h"
#include "macho.h"
#include "serial.h"
#include "riscv_efi_boot.h"

EFI_PHYSICAL_ADDRESS g_xnu_bootinfo_base;

#define PAGE_4K 0x1000ULL
#define SIZE_2M 0x200000ULL
#define SIZE_1G 0x40000000ULL
// the kernel collection has to stay inside the top 2GB, where medany code reaches
#define KC_WINDOW_LO 0xffffffff80000000ULL
#define FDT_MAGIC 0xd00dfeedU

static const EFI_GUID dtb_table_guid = { 0xb1b621d5, 0xf19c, 0x41a5,
                                         { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };

typedef struct {
  UINT64 lo, hi;
} Span;

static UINT64 align_up(UINT64 v, UINT64 a) {
  return (v + a - 1) & ~(a - 1);
}

static UINT32 be32(const UINT8 *p) {
  return (UINT32)p[0] << 24 | (UINT32)p[1] << 16 | (UINT32)p[2] << 8 | p[3];
}

static EFI_STATUS get_memory_map(AppContext *ctx, EFI_MEMORY_DESCRIPTOR **out, UINTN *count,
                                 UINTN *desc_size, UINTN *key) {
  UINTN size = 0;
  UINT32 version = 0;
  EFI_MEMORY_DESCRIPTOR *map = NULL;
  uefi_call_wrapper(ctx->bs->GetMemoryMap, 5, &size, NULL, key, desc_size, &version);
  size += 8 * *desc_size;
  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiLoaderData, size,
                                        (VOID **)&map);
  if (EFI_ERROR(status))
    return status;
  status = uefi_call_wrapper(ctx->bs->GetMemoryMap, 5, &size, map, key, desc_size, &version);
  if (EFI_ERROR(status)) {
    uefi_call_wrapper(ctx->bs->FreePool, 1, map);
    return status;
  }
  *out = map;
  *count = size / *desc_size;
  return EFI_SUCCESS;
}

static EFI_MEMORY_DESCRIPTOR *desc_at(EFI_MEMORY_DESCRIPTOR *map, UINTN desc_size, UINTN i) {
  return (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)map + i * desc_size);
}

// grows [lo, hi) over its neighbours, firmware reservations only when asked, the map need not be sorted
static void grow_span(EFI_MEMORY_DESCRIPTOR *map, UINTN n, UINTN desc_size, Span *s,
                      BOOLEAN firmware_ok) {
  BOOLEAN grew = TRUE;
  while (grew) {
    grew = FALSE;
    for (UINTN i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *d = desc_at(map, desc_size, i);
      UINT64 lo = d->PhysicalStart, hi = lo + (d->NumberOfPages << EFI_PAGE_SHIFT);
      if (!firmware_ok && d->Type == EfiReservedMemoryType)
        continue;
      if (hi == s->lo) {
        s->lo = lo;
        grew = TRUE;
      } else if (lo == s->hi) {
        s->hi = hi;
        grew = TRUE;
      }
    }
  }
}

// the kernel owns [physBase, end of window), so the window is the largest stretch of
// ram without a firmware reservation, and the bank is all the ram around it
static EFI_STATUS find_ram(AppContext *ctx, Span *window, Span *bank) {
  EFI_MEMORY_DESCRIPTOR *map;
  UINTN n, desc_size, key;
  EFI_STATUS status = get_memory_map(ctx, &map, &n, &desc_size, &key);
  if (EFI_ERROR(status))
    return status;

  window->lo = window->hi = 0;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d = desc_at(map, desc_size, i);
    if (d->Type == EfiReservedMemoryType)
      continue;
    Span s = { d->PhysicalStart, d->PhysicalStart + (d->NumberOfPages << EFI_PAGE_SHIFT) };
    grow_span(map, n, desc_size, &s, FALSE);
    if (s.hi - s.lo > window->hi - window->lo)
      *window = s;
  }
  *bank = *window;
  grow_span(map, n, desc_size, bank, TRUE);
  uefi_call_wrapper(ctx->bs->FreePool, 1, map);
  return window->hi > window->lo ? EFI_SUCCESS : EFI_NOT_FOUND;
}

// the segments the loader copies, same rule as macho_load_segments_contiguous
static EFI_STATUS collection_range(MachoImage *image, UINT64 *lo, UINT64 *hi) {
  macho_header_64 *hdr = image->header;
  UINT8 *p = (UINT8 *)(hdr + 1);
  *lo = ~0ULL;
  *hi = 0;
  for (UINT32 i = 0; i < hdr->ncmds; i++) {
    macho_load_command *lc = (macho_load_command *)p;
    if ((UINT8 *)(lc + 1) > (UINT8 *)image->data + image->size ||
        lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > (UINT8 *)image->data + image->size)
      return EFI_COMPROMISED_DATA;
    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      if (seg->vmsize && !(seg->vmaddr == 0 && seg->filesize == 0)) {
        if (seg->vmaddr < *lo)
          *lo = seg->vmaddr;
        if (seg->vmaddr + seg->vmsize > *hi)
          *hi = seg->vmaddr + seg->vmsize;
      }
    }
    p += lc->cmdsize;
  }
  return *lo < *hi ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static const CHAR8 *read_boot_args(AppContext *ctx) {
  const CHAR8 *fallback = (const CHAR8 *)"-v debug=0x8 serial=3 keepsyms=1";
  FileBuffer file = {0};
  EFI_STATUS status = file_read_all_from_any_volume(ctx, L"\\EFI\\BOOT\\boot-args.txt",
                                                    &file, NULL);
  if (EFI_ERROR(status) || file.size == 0) {
    log_info(L"no boot-args.txt (%r), using \"%a\"\r\n", status, fallback);
    return fallback;
  }
  UINTN len = file.size;
  CHAR8 *bytes = (CHAR8 *)file.data;
  while (len > 0 && (bytes[len - 1] == '\n' || bytes[len - 1] == '\r' ||
                     bytes[len - 1] == ' ' || bytes[len - 1] == '\t'))
    len--;
  CHAR8 *copy = NULL;
  if (len == 0 || EFI_ERROR(app_alloc_pool(ctx, len + 1, (VOID **)&copy))) {
    file_free(ctx, &file);
    return fallback;
  }
  CopyMem(copy, bytes, len);
  copy[len] = 0;
  file_free(ctx, &file);
  log_info(L"boot-args.txt: \"%a\"\r\n", copy);
  return copy;
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

static EFI_STATUS exit_boot_services(AppContext *ctx) {
  for (UINTN attempt = 0; attempt < 4; attempt++) {
    EFI_MEMORY_DESCRIPTOR *map;
    UINTN n, desc_size, key;
    EFI_STATUS status = get_memory_map(ctx, &map, &n, &desc_size, &key);
    if (EFI_ERROR(status))
      return status;
    // the pool stays allocated, freeing it would change the key again
    status = uefi_call_wrapper(ctx->bs->ExitBootServices, 2, ctx->image_handle, key);
    if (!EFI_ERROR(status))
      return EFI_SUCCESS;
  }
  return EFI_INVALID_PARAMETER;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  FileBuffer kernel = {0}, ramdisk = {0};
  MachoImage image_info;
  MachoLoadResult load = {0};
  EFI_STATUS status;

  SetMem(&ctx, sizeof(ctx), 0);
  status = app_init(&ctx, image, st);
  if (EFI_ERROR(status))
    return status;
  log_info(L"XNU EFI loader start (riscv64)\r\n");

  {
    static EFI_GUID guid = RISCV_EFI_BOOT_PROTOCOL_GUID;
    RISCV_EFI_BOOT_PROTOCOL *boot = NULL;
    UINTN hartid = 0;
    status = uefi_call_wrapper(ctx.bs->LocateProtocol, 3, &guid, NULL, (VOID **)&boot);
    if (EFI_ERROR(status) || !boot ||
        EFI_ERROR(uefi_call_wrapper(boot->GetBootHartId, 2, boot, &hartid))) {
      log_error(L"no RISCV_EFI_BOOT_PROTOCOL, the boot hart is unknown\r\n");
      return EFI_UNSUPPORTED;
    }
    ctx.boot_hartid = hartid;
    log_info(L"boot hart %lu\r\n", ctx.boot_hartid);
  }

  const UINT8 *fdt = find_fdt(&ctx);
  if (!fdt) {
    log_error(L"no flattened device tree from firmware\r\n");
    return EFI_NOT_FOUND;
  }
  UINT64 fdt_size = be32(fdt + 4);

  status = file_read_all_from_any_volume(&ctx, L"\\EFI\\BOOT\\kernel", &kernel, &ctx.boot_volume);
  if (EFI_ERROR(status)) {
    log_error(L"no kernel collection at \\EFI\\BOOT\\kernel: %r\r\n", status);
    return status;
  }
  log_info(L"kernel collection: %lu bytes\r\n", (UINT64)kernel.size);

  status = macho_parse(kernel.data, kernel.size, &image_info);
  if (EFI_ERROR(status) || image_info.header->cputype != CPU_TYPE_RISCV64) {
    log_error(L"not a riscv64 Mach-O (%r, cputype 0x%x)\r\n", status,
              EFI_ERROR(status) ? 0 : (UINT32)image_info.header->cputype);
    return EFI_LOAD_ERROR;
  }
  macho_dump(&image_info);

  UINT64 vm_lo, vm_hi, entry_vm;
  status = collection_range(&image_info, &vm_lo, &vm_hi);
  if (EFI_ERROR(status)) {
    log_error(L"no loadable segments: %r\r\n", status);
    return status;
  }
  status = macho_find_entry_vmaddr(&image_info, &entry_vm);
  if (EFI_ERROR(status) || entry_vm < vm_lo || entry_vm >= vm_hi) {
    log_error(L"no _start in the collection's LC_UNIXTHREAD: %r\r\n", status);
    return EFI_LOAD_ERROR;
  }
  vm_lo &= ~(PAGE_4K - 1);
  UINT64 span = align_up(vm_hi, PAGE_4K) - vm_lo;
  log_info(L"collection vm 0x%lx - 0x%lx, entry 0x%lx\r\n", vm_lo, vm_hi, entry_vm);

  const CHAR8 *cmdline = read_boot_args(&ctx);

  status = file_read_all_from_any_volume(&ctx, L"\\ramdisk.img", &ramdisk, NULL);
  if (EFI_ERROR(status))
    ramdisk.size = 0;

  // collection, then the boot-info block (boot_args and the device tree), the fdt copy, the ramdisk
  UINT64 bootinfo_size = XNU_BOOTINFO_END - XNU_BOOTINFO_BASE;
  UINT64 fdt_space = align_up(fdt_size, PAGE_4K);
  UINT64 ramdisk_space = align_up(ramdisk.size, PAGE_4K);
  UINT64 total = span + bootinfo_size + fdt_space + ramdisk_space;

  Span window, bank;
  status = find_ram(&ctx, &window, &bank);
  if (EFI_ERROR(status)) {
    log_error(L"no usable ram in the memory map: %r\r\n", status);
    return status;
  }
  log_info(L"ram bank 0x%lx - 0x%lx, kernel window 0x%lx - 0x%lx\r\n", bank.lo, bank.hi,
           window.lo, window.hi);

  // physBase keeps the collection's offset within 2MB so the slide is a 2MB multiple,
  // and the lowest slot the loader, the initrd and the fdt leave free wins
  UINT64 phys_base = 0;
  for (UINT64 try = align_up(window.lo, SIZE_2M) + (vm_lo & (SIZE_2M - 1));
       try + total <= window.hi; try += SIZE_2M) {
    EFI_PHYSICAL_ADDRESS at = try;
    if (!EFI_ERROR(uefi_call_wrapper(ctx.bs->AllocatePages, 4, AllocateAddress, EfiLoaderData,
                                     (UINTN)(total >> EFI_PAGE_SHIFT), &at))) {
      phys_base = try;
      break;
    }
  }
  if (!phys_base) {
    log_error(L"no room for 0x%lx bytes of kernel data in the window\r\n", total);
    return EFI_OUT_OF_RESOURCES;
  }
  SetMem((VOID *)(UINTN)phys_base, total, 0);

  // D = virtBase - physBase has to be a 1GB multiple, start.s maps the kernel with gigapages
  UINT64 slide = (phys_base - vm_lo) & (SIZE_1G - 1);
  if (vm_hi + slide < vm_hi && vm_lo + slide - SIZE_1G >= KC_WINDOW_LO)
    slide -= SIZE_1G;
  if (vm_lo + slide < KC_WINDOW_LO || vm_hi + slide - 1 < vm_lo + slide) {
    log_error(L"slide 0x%lx moves the collection out of the top 2GB\r\n", slide);
    return EFI_UNSUPPORTED;
  }
  UINT64 virt_base = vm_lo + slide;
  UINT64 delta = virt_base - phys_base;
  log_info(L"physBase 0x%lx virtBase 0x%lx slide 0x%lx delta 0x%lx\r\n", phys_base, virt_base,
           slide, delta);

  ctx.kernel_region_base = phys_base;
  ctx.kernel_region_end = phys_base + span;
  ctx.phys_base = phys_base;
  status = macho_load_segments_contiguous(&ctx, &image_info, phys_base, &load);
  if (EFI_ERROR(status) || load.lowest_vmaddr < vm_lo ||
      load.lowest_vmaddr - vm_lo >= PAGE_4K) {
    log_error(L"loading the collection failed: %r\r\n", status);
    return EFI_LOAD_ERROR;
  }
  // macho_load_segments_contiguous places from the lowest vmaddr, which vm_lo only page aligns
  if (load.lowest_vmaddr != vm_lo) {
    log_error(L"lowest segment 0x%lx is not page aligned\r\n", load.lowest_vmaddr);
    return EFI_LOAD_ERROR;
  }
  UINT64 entry_phys = phys_base + (entry_vm - vm_lo);
  log_info(L"collection loaded: %lu segments at phys 0x%lx, _start vm 0x%lx phys 0x%lx\r\n",
           (UINT64)load.segment_count, phys_base, entry_vm + slide, entry_phys);
  file_free(&ctx, &kernel);

  // the bootinfo block goes back to the allocator so dt_build and the boot_args
  // builder can claim their fixed pages inside it
  g_xnu_bootinfo_base = phys_base + span;
  uefi_call_wrapper(ctx.bs->FreePages, 2, g_xnu_bootinfo_base,
                    (UINTN)(bootinfo_size >> EFI_PAGE_SHIFT));

  ctx.fdt_copy_phys = XNU_BOOTINFO_END;
  ctx.fdt_copy_size = fdt_size;
  CopyMem((VOID *)(UINTN)ctx.fdt_copy_phys, fdt, fdt_size);
  // readers of the configuration table from here on, /efi included, see the copy the kernel keeps
  for (UINTN i = 0; i < ctx.st->NumberOfTableEntries; i++) {
    EFI_CONFIGURATION_TABLE *e = &ctx.st->ConfigurationTable[i];
    if (!CompareMem(&e->VendorGuid, &dtb_table_guid, sizeof(EFI_GUID)))
      e->VendorTable = (VOID *)(UINTN)ctx.fdt_copy_phys;
  }
  if (ramdisk.size) {
    ctx.ramdisk_phys = ctx.fdt_copy_phys + fdt_space;
    ctx.ramdisk_size = ramdisk_space;
    CopyMem((VOID *)(UINTN)ctx.ramdisk_phys, ramdisk.data, ramdisk.size);
    file_free(&ctx, &ramdisk);
    log_info(L"ramdisk.img at 0x%lx size 0x%lx\r\n", ctx.ramdisk_phys, ctx.ramdisk_size);
  }
  UINT64 top_of_kernel_data = phys_base + total;
  ctx.dram_base = bank.lo;
  ctx.dram_size = bank.hi - bank.lo;
  UINT64 mem_size = window.hi - phys_base;

  VOID *dt = NULL;
  UINT32 dt_size = 0;
  LowMemBuffer dt_buf = {0};
  status = dt_build(&ctx, &dt, &dt_size, &dt_buf, cmdline, 0);
  if (EFI_ERROR(status)) {
    log_error(L"device tree: %r\r\n", status);
    return status;
  }

  LowMemBuffer args_buf = {0};
  arm64_boot_args *args = NULL;
  status = arm64_boot_build_args(&ctx, cmdline, virt_base, phys_base, mem_size,
                                 top_of_kernel_data, (UINT64)(UINTN)dt, dt_size, &args_buf,
                                 &args);
  if (EFI_ERROR(status)) {
    log_error(L"boot_args: %r\r\n", status);
    return status;
  }
  args->memSizeActual = mem_size;
  arm64_boot_log_args(args);
  log_info(L"  memSizeActual: 0x%lx  dram 0x%lx + 0x%lx  hart %lu\r\n", args->memSizeActual,
           ctx.dram_base, ctx.dram_size, ctx.boot_hartid);

  if ((delta & (SIZE_1G - 1)) != 0 || (UINT64)(UINTN)args < phys_base ||
      (UINT64)(UINTN)args + sizeof(*args) > top_of_kernel_data ||
      (UINT64)(UINTN)dt + dt_size > top_of_kernel_data) {
    log_error(L"handoff layout check failed\r\n");
    return EFI_ABORTED;
  }

  log_info(L"jumping to _start at phys 0x%lx, a0 = 0x%lx, a1 = %lu\r\n", entry_phys,
           (UINT64)(UINTN)args, ctx.boot_hartid);
  status = exit_boot_services(&ctx);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices: %r\r\n", status);
    return status;
  }
  serial_mark((CONST CHAR8 *)"exit_boot_services returned, entering the kernel");
  riscv64_jump_to_xnu(entry_phys, (UINT64)(UINTN)args, ctx.boot_hartid);
}
