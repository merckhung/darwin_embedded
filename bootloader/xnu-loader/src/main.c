#include "common.h"
#include "app.h"
#include "boot.h"
#include "console.h"
#include "fileio.h"
#include "macho.h"
#include "jump.h"
#include "serial.h"
#include "platform.h"

static VOID   *g_jump_entry;
static UINT64  g_jump_args;
static VOID   *g_jump_stack;
static UINT64  g_copy_src;
static UINT64  g_copy_dst;
static UINT64  g_copy_bytes;

static BOOLEAN boot_args_has_rd(const CHAR8 *args) {
  if (!args)
    return FALSE;
  for (const CHAR8 *p = args; *p; p++) {
    if ((p == args || p[-1] == ' ') && p[0] == 'r' && p[1] == 'd' && p[2] == '=')
      return TRUE;
  }
  return FALSE;
}

// held at power-on like a Mac's command keys: V boots verbose with the text console kept live, S single-user
// keys typed during the firmware are buffered, so a short poll catches them
static VOID boot_keys_apply(AppContext *ctx, CONST CHAR8 **cmdline, BOOLEAN *owned) {
  EFI_INPUT_KEY key;
  BOOLEAN verbose = FALSE, single = FALSE;
  CHAR8 *copy = NULL;
  UINTN len = 0, i, extra;

  if (ST == NULL || ST->ConIn == NULL)
    return;
  for (i = 0; i < 25; i++) {
    while (!EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key))) {
      if (key.UnicodeChar == L'v' || key.UnicodeChar == L'V')
        verbose = TRUE;
      if (key.UnicodeChar == L's' || key.UnicodeChar == L'S')
        single = TRUE;
    }
    uefi_call_wrapper(ctx->bs->Stall, 1, 20000);
  }

  verbose = verbose && !boot_cmdline_has_flag(*cmdline, (const CHAR8 *)"-v");
  single = single && !boot_cmdline_has_flag(*cmdline, (const CHAR8 *)"-s");
  if (!verbose && !single)
    return;
  while ((*cmdline)[len] != '\0')
    len++;
  extra = (verbose ? 16 : 0) + (single ? 3 : 0);
  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiLoaderData, len + extra + 1, (VOID **)&copy)))
    return;
  CopyMem(copy, *cmdline, len);
  if (verbose) {
    CopyMem(copy + len, " -v gopconsole=1", 16);
    len += 16;
  }
  if (single) {
    CopyMem(copy + len, " -s", 3);
    len += 3;
  }
  copy[len] = '\0';
  if (*owned)
    app_free_pool(ctx, (VOID *)(UINTN)*cmdline);
  *cmdline = copy, *owned = TRUE;
  log_info(L"boot keys: using \"%a\"\r\n", copy);
}

static EFI_STATUS append_ramdisk_boot_arg(
    AppContext *ctx, const CHAR8 **cmdline, BOOLEAN *owned) {
  const CHAR8 suffix[] = " rd=md0 rootfstype=hfs";
  UINTN len = 0;
  CHAR8 *copy = NULL;

  if (!ctx || !cmdline || !*cmdline || !owned)
    return EFI_INVALID_PARAMETER;
  if (boot_args_has_rd(*cmdline))
    return EFI_SUCCESS;

  while ((*cmdline)[len] != '\0')
    len++;
  UINTN suffix_len = sizeof(suffix) - 1;
  EFI_STATUS status = app_alloc_pool(ctx, len + suffix_len + 1, (VOID **)&copy);
  if (EFI_ERROR(status))
    return status;

  CopyMem(copy, *cmdline, len);
  CopyMem(copy + len, suffix, suffix_len + 1);
  if (*owned)
    app_free_pool(ctx, (VOID *)(UINTN)*cmdline);
  *cmdline = copy;
  *owned = TRUE;
  return EFI_SUCCESS;
}

#if defined(__aarch64__)
// Apple's trust caches, merged and laid out as XNU reads them from /chosen/memory-map/TrustCache.
// Without the file the loader keeps publishing an empty static module
static VOID load_trustcache(AppContext *ctx) {
  EFI_FILE_PROTOCOL *root = NULL;
  FileBuffer file = {0};
  EFI_STATUS status;

  ctx->trustcache_slack = XNU_BOOTINFO_ALIGN;
  status = app_open_self_volume(ctx, &root);
  if (!EFI_ERROR(status)) {
    status = file_read_all(ctx, root, L"\\trustcache.bin", &file);
    uefi_call_wrapper(root->Close, 1, root);
  }
  if (EFI_ERROR(status) || file.size < 8) {
    log_info(L"no trustcache.bin (%r); using an empty static trust cache\r\n", status);
    return;
  }
  ctx->trustcache_data = file.data;
  ctx->trustcache_size = file.size;
  ctx->trustcache_slack = (file.size + XNU_BOOTINFO_ALIGN - 1) & ~(XNU_BOOTINFO_ALIGN - 1);
  log_info(L"trustcache.bin: %lu bytes\r\n", (UINT64)file.size);
}
#endif

static EFI_STATUS load_ramdisk(AppContext *ctx) {
  EFI_FILE_PROTOCOL *root = NULL;
  FileBuffer image = {0};
  LowMemBuffer storage = {0};
  EFI_STATUS status;

  status = app_open_self_volume(ctx, &root);
  if (!EFI_ERROR(status)) {
    status = file_read_all(ctx, root, L"\\ramdisk.img", &image);
    uefi_call_wrapper(root->Close, 1, root);
  }

  if (EFI_ERROR(status)) {
    log_info(L"no ramdisk.img found (%r); continuing without RAMDisk\r\n", status);
    return EFI_SUCCESS;
  }

  if (image.size == 0) {
    file_free(ctx, &image);
    log_info(L"ramdisk.img is empty; continuing without RAMDisk\r\n");
    return EFI_SUCCESS;
  }

#if defined(__aarch64__)
  // arm64 xnu hands everything above topOfKernelData to the vm, so keep the ramdisk
  // right after the boot-info block where topOfKernelData can cover it like iboot does
  {
    UINTN pages = (image.size + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT;
    for (EFI_PHYSICAL_ADDRESS at = XNU_BOOTINFO_END;
         at < XNU_BOOTINFO_END + 64ULL * XNU_L2_BLOCK_SIZE;
         at = (at + XNU_L2_BLOCK_SIZE) & ~(XNU_L2_BLOCK_SIZE - 1)) {
      EFI_PHYSICAL_ADDRESS try_at = at;
      if (!EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePages, 4,
              AllocateAddress, EfiLoaderData, pages, &try_at))) {
        storage.ptr = (VOID *)(UINTN)try_at;
        storage.phys = try_at;
        storage.size = image.size;
        storage.pages = pages;
        break;
      }
    }
  }
  status = storage.ptr ? EFI_SUCCESS
                       : lowmem_alloc_pages(ctx, image.size, EfiLoaderData, &storage);
#else
  /* Keep the RAM disk out of XNU's available-page pool. Its physical-memory
   * device accesses these pages through the physmap, not the static mapping. */
  status = lowmem_alloc_pages(ctx, image.size, EfiReservedMemoryType, &storage);
#endif
  if (EFI_ERROR(status)) {
    file_free(ctx, &image);
    log_error(L"failed to allocate RAMDisk pages: %r\r\n", status);
    return status;
  }

  CopyMem(storage.ptr, image.data, image.size);
  file_free(ctx, &image);
  ctx->ramdisk_phys = storage.phys;
  ctx->ramdisk_size = (UINT64)(storage.pages << EFI_PAGE_SHIFT);
  log_info(L"loaded ramdisk.img RAMDisk phys=0x%lx size=0x%lx\r\n",
           (UINT64)ctx->ramdisk_phys, ctx->ramdisk_size);
  return EFI_SUCCESS;
}

#if defined(PD_ARCH_X86)
static VOID finish_boot_and_jump(VOID *unused) {
  UINT64 *src64 = (UINT64 *)(UINTN)g_copy_src;
  UINT64 *dst64 = (UINT64 *)(UINTN)g_copy_dst;
  UINTN   words = (UINTN)((g_copy_bytes + 7) >> 3);

  (VOID)unused;

  serial_trace((CONST CHAR8 *)"copy src   ", g_copy_src);
  serial_trace((CONST CHAR8 *)"copy dst   ", g_copy_dst);
  serial_trace((CONST CHAR8 *)"copy end   ", g_copy_dst + g_copy_bytes);
  serial_trace((CONST CHAR8 *)"loader sp  ", (UINT64)(UINTN)&words);

  for (UINTN i = 0; i < words; i++)
    dst64[i] = src64[i];

  /* Low memory can be shadowed, decoded elsewhere or simply not backed by RAM
   * on real hardware, in which case the stores above are dropped and the kernel
   * we jump to is whatever was already there. */
  if (dst64[0] != src64[0] || dst64[words - 1] != src64[words - 1]) {
    serial_mark((CONST CHAR8 *)"FATAL kernel copy did not land");
    for (;;) { }
  }
  serial_mark((CONST CHAR8 *)"copy verified");
  serial_mark((CONST CHAR8 *)"jumping to kernel");

  jump_to_xnu(g_jump_entry, g_jump_args, g_jump_stack);
}
#endif

#if defined(__aarch64__)
EFI_PHYSICAL_ADDRESS g_xnu_bootinfo_base;

/* The kernel starts with the MMU and caches off, so its reads bypass the D-cache.
 * Anything written here that is still dirty in cache is invisible to it (under KVM
 * that showed up as firmware page tables on the exception vector page) */
static VOID arm64_clean_to_poc(UINT64 base, UINT64 size) {
  UINT64 ctr, line;

  if (size == 0)
    return;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  line = 4ULL << ((ctr >> 16) & 0xf);
  for (UINT64 a = base & ~(line - 1); a < base + size; a += line)
    __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
  __asm__ volatile("dsb sy" : : : "memory");
}
#endif


static EFI_STATUS ReserveBootInfoGuard(AppContext *ctx,
                                       EFI_PHYSICAL_ADDRESS *guard_base,
                                       UINTN *guard_pages) {
  EFI_PHYSICAL_ADDRESS base = XNU_BOOTINFO_BASE;
  UINTN pages = (UINTN)((XNU_BOOTINFO_END - XNU_BOOTINFO_BASE) >> EFI_PAGE_SHIFT);

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, pages, &base);
  if (EFI_ERROR(status)) {
    log_error(L"boot-info guard: AllocateAddress(0x%lx, %lu pages) failed: %r\r\n",
              (UINT64)XNU_BOOTINFO_BASE, (UINT64)pages, status);
    return status;
  }

  *guard_base = base;
  *guard_pages = pages;
  log_info(L"boot-info guard: reserved 0x%lx - 0x%lx\r\n",
           (UINT64)base,
           (UINT64)(base + ((UINT64)pages << EFI_PAGE_SHIFT)));
  return EFI_SUCCESS;
}

static VOID ReleaseBootInfoGuard(AppContext *ctx,
                                 EFI_PHYSICAL_ADDRESS *guard_base,
                                 UINTN *guard_pages) {
  if (*guard_pages == 0)
    return;

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->FreePages, 2,
      *guard_base, *guard_pages);
  if (EFI_ERROR(status)) {
    log_error(L"boot-info guard: FreePages(0x%lx, %lu pages) failed: %r\r\n",
              (UINT64)*guard_base, (UINT64)*guard_pages, status);
    return;
  }

  log_info(L"boot-info guard: released for boot_build_args\r\n");
  *guard_base = 0;
  *guard_pages = 0;
}

#if defined(__aarch64__)
/* XNU manages one contiguous range from physBase, so it gets the largest run
 * of RAM without firmware holes, like sunxi's /memreserve/ secure monitor */
static UINT64 g_xnu_window_lo, g_xnu_window_hi;

/* The types app_detect_physical_memory_size counts as RAM */
static BOOLEAN is_ram_type(UINT32 type) {
  return type == EfiLoaderCode || type == EfiLoaderData ||
         type == EfiBootServicesCode || type == EfiBootServicesData ||
         type == EfiRuntimeServicesCode || type == EfiRuntimeServicesData ||
         type == EfiConventionalMemory || type == EfiACPIReclaimMemory ||
         type == EfiACPIMemoryNVS || type == EfiPalCode;
}

static VOID FindXnuWindow(AppContext *ctx) {
  UINTN map_size = 0, key = 0, desc_size = 0;
  UINT32 desc_ver = 0;
  EFI_MEMORY_DESCRIPTOR *mm = NULL;

  uefi_call_wrapper(ctx->bs->GetMemoryMap, 5, &map_size, mm, &key, &desc_size, &desc_ver);
  map_size += desc_size * 4;

  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->AllocatePool, 3, EfiLoaderData, map_size, (VOID **)&mm)) || !mm)
    return;

  if (EFI_ERROR(uefi_call_wrapper(ctx->bs->GetMemoryMap, 5, &map_size, mm, &key, &desc_size, &desc_ver))) {
    uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
    return;
  }

  UINTN n = map_size / desc_size;
  for (UINTN i = 0; i < n; i++) {
    EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + i * desc_size);
    UINT64 lo = d->PhysicalStart, hi = lo + (d->NumberOfPages << EFI_PAGE_SHIFT);
    BOOLEAN grew = TRUE;

    if (!is_ram_type(d->Type))
      continue;

    /* The map need not be sorted: keep appending whichever RAM range starts at hi */
    while (grew) {
      grew = FALSE;
      for (UINTN j = 0; j < n; j++) {
        EFI_MEMORY_DESCRIPTOR *e = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)mm + j * desc_size);
        if (is_ram_type(e->Type) && e->PhysicalStart == hi) {
          hi += e->NumberOfPages << EFI_PAGE_SHIFT;
          grew = TRUE;
        }
      }
    }

    if (hi - lo > g_xnu_window_hi - g_xnu_window_lo) {
      g_xnu_window_lo = lo;
      g_xnu_window_hi = hi;
    }
  }

  uefi_call_wrapper(ctx->bs->FreePool, 1, mm);
  log_info(L"xnu window: 0x%lx - 0x%lx\r\n", g_xnu_window_lo, g_xnu_window_hi);
}
#endif

EFI_STATUS AllocKernelMemRegion(AppContext *ctx, UINT64 span_bytes, UINT64 virt_base) {
#if defined(__aarch64__)
  UINT64 required_rem = virt_base & (XNU_L2_BLOCK_SIZE - 1);
  UINT64 first = XNU_LOADER_RAM_BASE + XNU_L2_BLOCK_SIZE + required_rem;

  FindXnuWindow(ctx);

  /* Start inside the window, leaving room below for the trust caches */
  UINT64 tc_slack = ctx->trustcache_slack ? ctx->trustcache_slack : XNU_BOOTINFO_ALIGN;
  if (g_xnu_window_lo > XNU_LOADER_RAM_BASE) {
    UINT64 w = (g_xnu_window_lo + tc_slack + XNU_L2_BLOCK_SIZE - 1) & ~(XNU_L2_BLOCK_SIZE - 1);
    if (w + required_rem > first)
      first = w + required_rem;
  }
  /* Two slacks: one below base for the trust caches,
   * one above to keep the bootinfo block inside this allocation. */
  UINTN total_pages = (UINTN)((span_bytes +
                               (XNU_BOOTINFO_END - XNU_BOOTINFO_BASE) +
                               tc_slack + XNU_BOOTINFO_ALIGN +
                               EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);
  EFI_PHYSICAL_ADDRESS base = 0;
  EFI_STATUS status = EFI_NOT_FOUND;
  for (UINT64 try = first; try < first + 64ULL * XNU_L2_BLOCK_SIZE; try += XNU_L2_BLOCK_SIZE) {
    /* Claim the trustcache page below the image as part of this allocation;
     * grabbing it separately afterwards fails whenever UEFI already owns it. */
    base = try - tc_slack;
    status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, total_pages, &base);
    if (!EFI_ERROR(status)) {
      base = try;
      break;
    }
  }
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion: no low-RAM slot for %lu pages: %r\r\n",
              (UINT64)total_pages, status);
    return status;
  }

  SetMem((VOID *)(UINTN)(base - tc_slack),
         (UINTN)((UINT64)total_pages << EFI_PAGE_SHIFT), 0);

  ctx->kernel_region_base = base;
  ctx->kernel_region_end  = base + span_bytes;

  log_info(L"kernel staging: 0x%lx - 0x%lx (%lu pages, low-RAM, virt_base rem 0x%lx)\r\n",
       ctx->kernel_region_base, ctx->kernel_region_end, (UINT64)total_pages, required_rem);
  return EFI_SUCCESS;
#else
  (VOID)virt_base;
  EFI_PHYSICAL_ADDRESS base = 0x7FFFFFFF;
  UINTN total_pages = (UINTN)((span_bytes + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);

  EFI_STATUS status = uefi_call_wrapper(ctx->bs->AllocatePages, 4,
    AllocateMaxAddress, EfiLoaderData, total_pages, &base);
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion: AllocatePages(%lu pages) failed: %r\r\n",
              (UINT64)total_pages, status);
    return status;
  }

  SetMem((VOID *)(UINTN)base, (UINTN)((UINT64)total_pages << EFI_PAGE_SHIFT), 0);

  ctx->kernel_region_base = base;
  ctx->kernel_region_end  = base + ((UINT64)total_pages << EFI_PAGE_SHIFT);

  log_info(L"kernel staging: 0x%lx - 0x%lx (%lu pages)\r\n",
       ctx->kernel_region_base, ctx->kernel_region_end, (UINT64)total_pages);
  return EFI_SUCCESS;
#endif
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
  AppContext ctx;
  FileBuffer kernel = {0};
  MachoImage image_info;
  MachoLoadResult load_result = {0};
  EFI_STATUS status;
  EFI_PHYSICAL_ADDRESS bootinfo_guard_base = 0;
  UINTN bootinfo_guard_pages = 0;

  status = app_init(&ctx, image, st);
  if (EFI_ERROR(status))
    return status;

  // boot args and boot keys come first, so a graphical boot is quiet before anything prints
  CONST CHAR8 *cmdline = "-v debug=0x219 -nogzalloc_mode keepsyms=1 serial=3 gopconsole=1";
  BOOLEAN cmdline_owned = FALSE;
  FileBuffer boot_args_file = {0};
  EFI_STATUS args_status = file_read_all_from_any_volume(
      &ctx,
      L"\\EFI\\BOOT\\boot-args.txt",
      &boot_args_file,
      NULL);

  if (!EFI_ERROR(args_status) && boot_args_file.size > 0) {
    UINTN len = boot_args_file.size;
    CHAR8 *bytes = (CHAR8 *)boot_args_file.data;
    /* Trim trailing CR/LF/whitespace a text editor may have left. */
    while (len > 0 &&
           (bytes[len - 1] == '\n' || bytes[len - 1] == '\r' ||
            bytes[len - 1] == ' '  || bytes[len - 1] == '\t'))
      len--;

    if (len > 0) {
      CHAR8 *copy = NULL;
      status = uefi_call_wrapper(ctx.bs->AllocatePool, 3, EfiLoaderData, len + 1, (VOID **)&copy);
      if (!EFI_ERROR(status)) {
        CopyMem(copy, bytes, len);
        copy[len] = '\0';
        cmdline = copy;
        cmdline_owned = TRUE;
        log_info(L"boot-args.txt: using \"%a\"\r\n", cmdline);
      } else {
        log_error(L"boot-args.txt: AllocatePool failed (%r), using default\r\n", status);
      }
    }
    file_free(&ctx, &boot_args_file);
  } else {
    log_info(L"no boot-args.txt found (%r); using default boot args\r\n", args_status);
  }
  boot_keys_apply(&ctx, &cmdline, &cmdline_owned);
  if (!boot_cmdline_has_flag(cmdline, (const CHAR8 *)"-v"))
    log_screen_quiet();
  boot_save_panic_log(&ctx);

  log_info(L"XNU EFI loader start\r\n");

#if defined(__aarch64__)
  /*
   * CPU feature probe. The board's capabilities decide how the kernel must
   * be built, and they are not documented anywhere we trust:
   *
   *   ID_AA64MMFR0_EL1.TGran16 [23:20] - 0 means no 16KB translation
   *     granule, which is why this target needs a 4KB-page kernel.
   *   ID_AA64ISAR0_EL1.SHA2 [15:12] - 0 means no SHA-256 crypto extension,
   *     so XNU's accelerated corecrypto assembly cannot run here.
   *   ID_AA64ISAR0_EL1.AES [7:4] and .SHA1 [11:8] likewise.
   */
  {
    UINT64 mmfr0 = 0, isar0 = 0;
    __asm__ volatile ("mrs %0, ID_AA64MMFR0_EL1" : "=r"(mmfr0));
    __asm__ volatile ("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
    log_info(L"CPU ID_AA64MMFR0_EL1=0x%lx TGran4=%u TGran64=%u TGran16=%u\r\n",
             mmfr0,
             (UINT32)((mmfr0 >> 28) & 0xF),
             (UINT32)((mmfr0 >> 24) & 0xF),
             (UINT32)((mmfr0 >> 20) & 0xF));
    log_info(L"CPU ID_AA64ISAR0_EL1=0x%lx AES=%u SHA1=%u SHA2=%u\r\n",
             isar0,
             (UINT32)((isar0 >> 4) & 0xF),
             (UINT32)((isar0 >> 8) & 0xF),
             (UINT32)((isar0 >> 12) & 0xF));
  }
#endif

  {
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(app_get_loaded_image(&ctx, &li)))
      log_info(L"loader image base=0x%lx size=0x%lx\r\n",
               (UINT64)(UINTN)li->ImageBase, (UINT64)li->ImageSize);
  }

  EFI_HANDLE found_handle = NULL;

  status = file_read_all_from_any_volume(
      &ctx,
      L"\\EFI\\BOOT\\kernel",
      &kernel,
      &found_handle);

  if (EFI_ERROR(status)) {
    log_info(L"no local kernel file (%r); trying TFTP (netboot)\r\n", status);
    status = file_read_all_via_tftp(&ctx, (CONST CHAR8 *)"EFI/BOOT/kernel", &kernel);
  }

  if (EFI_ERROR(status)) {
    log_error(L"failed to read kernel from any volume or TFTP: %r\r\n", status);
    return status;
  }
  ctx.boot_volume = found_handle;

  log_info(L"kernel size: %lu bytes\r\n", kernel.size);

  status = macho_parse(kernel.data, kernel.size, &image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to parse Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_dump(&image_info);
  if (EFI_ERROR(status)) {
    log_error(L"failed to dump Mach-O: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  {
#ifdef KASLR_ENABLED
    ctx.kslide = 2 * KASLR_SLIDE_GRANULE;
    log_info(L"KASLR: fixed slide=0x%x\r\n", ctx.kslide);
#else
    ctx.kslide = 0;
    log_info(L"KASLR: disabled, kslide=0\r\n");
#endif
  }

  /* Compute vm range to derive phys_base before any allocation,
   * matching Apple: phys_base = lo32(lowest_vmaddr) + kslide */
  {
    UINT64 lo = 0, hi = 0;
    status = macho_compute_vm_range_pub(&image_info, &lo, &hi);
    if (EFI_ERROR(status)) {
      log_error(L"failed to compute vm range: %r\r\n", status);
      file_free(&ctx, &kernel);
      return status;
    }
    ctx.phys_base = (UINT32)lo;  /* physical is always unslid; kslide shifts VAs only */
    log_info(L"phys_base=0x%lx kslide=0x%x span=0x%lx\r\n",
             (UINT64)ctx.phys_base, ctx.kslide, hi - lo);

  }

#if !defined(__aarch64__)
  /* x86: the boot-info block lives at a fixed low address (0x3000000);
   * reserve it before the staging allocation can land on it. On arm64 the
   * block is carved out of the staging region itself, after it exists. */
  status = ReserveBootInfoGuard(&ctx, &bootinfo_guard_base, &bootinfo_guard_pages);
  if (EFI_ERROR(status)) {
    file_free(&ctx, &kernel);
    return status;
  }
#endif

#if defined(__aarch64__)
  load_trustcache(&ctx);
#endif

  /* Allocate a high staging buffer for the kernel image. */
  {
    UINT64 lo2 = 0, hi2 = 0;
    macho_compute_vm_range_pub(&image_info, &lo2, &hi2);
    status = AllocKernelMemRegion(&ctx, hi2 - lo2, lo2);
  }
  if (EFI_ERROR(status)) {
    log_error(L"AllocKernelMemRegion failed: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }
  /* phys_base = staging address; post-EBS copy moves it to 0x100000 */
  ctx.phys_base = ctx.kernel_region_base;

#if defined(__aarch64__)
  if (ctx.kernel_region_base < ctx.trustcache_slack) {
    log_error(L"trustcache: kernel staging address is too low\r\n");
    file_free(&ctx, &kernel);
    return EFI_OUT_OF_RESOURCES;
  }
  /* Already inside the staging allocation; see AllocKernelMemRegion. */
  ctx.trustcache_phys = ctx.kernel_region_base - ctx.trustcache_slack;
  log_info(L"arm64 trustcache page=0x%lx (below kernel)\r\n",
           (UINT64)ctx.trustcache_phys);

  g_xnu_bootinfo_base = (ctx.kernel_region_end + XNU_BOOTINFO_ALIGN - 1) &
                        ~(EFI_PHYSICAL_ADDRESS)(XNU_BOOTINFO_ALIGN - 1);
  log_info(L"arm64 bootinfo base=0x%lx (after kernel image)\r\n",
           (UINT64)g_xnu_bootinfo_base);
  status = uefi_call_wrapper(ctx.bs->FreePages, 2, g_xnu_bootinfo_base,
      (UINTN)((XNU_BOOTINFO_END - XNU_BOOTINFO_BASE) >> EFI_PAGE_SHIFT));
  if (EFI_ERROR(status)) {
    log_error(L"bootinfo: FreePages(0x%lx) failed: %r\r\n",
              (UINT64)g_xnu_bootinfo_base, status);
    file_free(&ctx, &kernel);
    return status;
  }

  status = ReserveBootInfoGuard(&ctx, &bootinfo_guard_base, &bootinfo_guard_pages);
  if (EFI_ERROR(status)) {
    file_free(&ctx, &kernel);
    return status;
  }
#endif

  /* Single contiguous allocation at phys_base, matching Apple's model */
  status = macho_load_segments_contiguous(&ctx, &image_info, ctx.phys_base, &load_result);
  if (EFI_ERROR(status)) {
    log_error(L"failed to load segments: %r\r\n", status);
    file_free(&ctx, &kernel);
    return status;
  }

  if (ctx.kslide) {
    status = macho_apply_kaslr_slide(&image_info, &load_result, ctx.kslide);
    if (EFI_ERROR(status)) {
      log_error(L"KASLR: apply slide failed: %r\r\n", status);
      macho_unload_contiguous(&ctx, &load_result);
      file_free(&ctx, &kernel);
      return status;
    }
    /* XNU rebuilds page tables from its embedded header; slide it too. */
    status = macho_patch_header_slide(&image_info, &load_result, ctx.kslide);
    if (EFI_ERROR(status)) {
      log_error(L"KASLR: patch embedded header failed: %r\r\n", status);
      macho_unload_contiguous(&ctx, &load_result);
      file_free(&ctx, &kernel);
      return status;
    }
  }

  log_info(
      L"loaded image phys_base=0x%lx vm_low=0x%lx span=0x%lx segments=%u\r\n",
      (UINT64)load_result.host_base,
      load_result.lowest_vmaddr,
      load_result.image_size,
      load_result.segment_count);

  UINT64 entry_vmaddr = 0;
  VOID *host_entry = NULL;

  status = macho_find_entry_vmaddr(&image_info, &entry_vmaddr);
  if (EFI_ERROR(status)) {
    log_error(L"failed to find entry vmaddr: %r\r\n", status);
    macho_unload_contiguous(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

  status = macho_compute_host_entry(&load_result, entry_vmaddr, &host_entry);
  if (EFI_ERROR(status)) {
    log_error(L"failed to compute host entry: %r\r\n", status);
    macho_unload_contiguous(&ctx, &load_result);
    file_free(&ctx, &kernel);
    return status;
  }

#ifdef VERBOSE_MACHO
  macho_log_entry_context(&image_info, entry_vmaddr);
  macho_log_section_host_info(&image_info, &load_result, "__HIB", "__bootPT");
  macho_dump_entry_bytes(host_entry, 32);
#endif // VERBOSE_MACHO

  log_info(L"entry vm=0x%lx -> host=0x%lx\r\n", entry_vmaddr, (UINT64)(UINTN)host_entry);

  status = load_ramdisk(&ctx);
  if (EFI_ERROR(status))
    return status;

  BootArgsState boot_state = {0};

  ReleaseBootInfoGuard(&ctx, &bootinfo_guard_base, &bootinfo_guard_pages);
  if (bootinfo_guard_pages != 0) {
    file_free(&ctx, &kernel);
    return EFI_DEVICE_ERROR;
  }

  status = boot_collect_memory_map(&ctx, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to collect memory map: %r\r\n", status);
    return status;
  }


  if (ctx.ramdisk_size != 0) {
    status = append_ramdisk_boot_arg(&ctx, &cmdline, &cmdline_owned);
    if (EFI_ERROR(status)) {
      log_error(L"failed to append rd=md0: %r\r\n", status);
      return status;
    }
  }

  status = boot_build_args(&ctx, cmdline, &load_result, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"failed to build boot_args: %r\r\n", status);
    return status;
  }

#if defined(__aarch64__)
  LowMemBuffer arm64_args_buf = {0};
  arm64_boot_args *arm64_args = NULL;
  {
    UINT64 arm64_phys_base = ctx.kernel_region_base - ctx.trustcache_slack;
    UINT64 arm64_physical_mem_size =
        (XNU_LOADER_RAM_BASE + app_detect_physical_memory_size(&ctx))
        - arm64_phys_base;
    /* Managed memory ends where the window does, before any firmware hole above it. The
     * detected size sums RAM around the holes below, so base + size falls short of the end */
    if (g_xnu_window_hi > arm64_phys_base)
      arm64_physical_mem_size = g_xnu_window_hi - arm64_phys_base;
    status = arm64_boot_build_args(
        &ctx,
        cmdline,
        load_result.lowest_vmaddr - ctx.trustcache_slack,  /* virtBase */
        arm64_phys_base,                         /* physBase - staging IS final for arm64 */
        arm64_physical_mem_size,
        XNU_BOOTINFO_END,                        /* topOfKernelData: covers boot-info block */
        (UINT64)(UINTN)boot_state.device_tree,
        boot_state.device_tree_size,
        &arm64_args_buf,
        &arm64_args);
    if (EFI_ERROR(status)) {
      log_error(L"failed to build arm64 boot_args: %r\r\n", status);
      return status;
    }
    /*
     * QEMU's ramfb is allocated from the top of system RAM. XNU derives its
     * managed-page aperture from physBase/memSize rather than the EFI memory
     * map, so including the framebuffer there makes later IOMemoryDescriptor
     * mappings treat device memory as managed RAM. End managed memory at the
     * framebuffer when firmware placed it inside that aperture.
     */
    UINT64 arm64_fb_phys = arm64_args->Video.v_baseAddr & ~3ULL;
    if (arm64_fb_phys > arm64_phys_base &&
        arm64_fb_phys <
            arm64_phys_base + arm64_physical_mem_size) {
      arm64_physical_mem_size =
          arm64_fb_phys - arm64_phys_base;
      arm64_args->memSize = arm64_physical_mem_size;
    }
    arm64_args->memSizeActual = arm64_physical_mem_size;
    // a ramdisk above the boot-info block must stay out of the vm's free pages
    if (ctx.ramdisk_size != 0 && ctx.ramdisk_phys >= XNU_BOOTINFO_END &&
        ctx.ramdisk_phys + ctx.ramdisk_size <= arm64_phys_base + arm64_physical_mem_size) {
      arm64_args->topOfKernelData =
          (ctx.ramdisk_phys + ctx.ramdisk_size + XNU_BOOTINFO_ALIGN - 1) & ~(XNU_BOOTINFO_ALIGN - 1);
    }
  }
#endif

  if (cmdline_owned)
    app_free_pool(&ctx, (VOID *)(UINTN)cmdline);

  EFI_PHYSICAL_ADDRESS stack_base = 0xFFFFFFFFULL;
  UINTN stack_pages = 16;

  status = uefi_call_wrapper(
      ctx.bs->AllocatePages, 4,
      AllocateMaxAddress,
      EfiLoaderData,
      stack_pages,
      &stack_base);
  if (EFI_ERROR(status)) {
    log_error(L"failed to allocate stack: %r\r\n", status);
    return status;
  }

  VOID *stack_top = (VOID *)((UINT8 *)(UINTN)stack_base +
                             (stack_pages << EFI_PAGE_SHIFT) - 16);

#ifdef VERBOSE_BOOT
#if defined(__aarch64__)
  arm64_boot_log_args(arm64_args);
#else
  boot_log_args(&boot_state);
#endif
#endif // VERBOSE_BOOT

  /* Reserve pages in the boot-info block (phys >= 0x100000) for the post-EBS
   * memory map relocation, so the map XNU walks survives pmap_lowmem_finalize
   * (see common.h XNU_BOOTINFO_BASE). 16 pages = 64KB, ample for the map. */
  EFI_PHYSICAL_ADDRESS mm_fixed = XNU_MEMMAP_PHYS;
  status = uefi_call_wrapper(ctx.bs->AllocatePages, 4,
      AllocateAddress, EfiLoaderData, 16, &mm_fixed);
  if (EFI_ERROR(status)) {
    log_error(L"reloc: failed to alloc mmap pages: %r\r\n", status);
    return status;
  }

#ifdef VERBOSE_BOOT
  log_info(L"[D] before exit_boot_services\r\n");
#if defined(__aarch64__)
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)arm64_args);
  log_info(L"handoff entry_vmaddr=0x%lx host_entry=0x%lx phys_entry=0x%lx\r\n",
           entry_vmaddr, (UINT64)(UINTN)host_entry, (UINT64)(UINTN)host_entry);
  log_info(L"handoff phys_real=0x%lx vm_base=0x%lx\r\n",
           (UINT64)ctx.kernel_region_base, load_result.lowest_vmaddr);
  log_info(L"handoff physBase=0x%lx topOfKernelData=0x%lx\r\n",
           arm64_args->physBase, arm64_args->topOfKernelData);
#else
  log_info(L"boot_args phys = 0x%lx\r\n", (UINT64)(UINTN)boot_state.args);
  {
    UINT64 handoff_phys_real = 0x100000ULL + ctx.kslide;
    INT64 handoff_vm_slide = (INT64)handoff_phys_real -
                             (INT64)load_result.lowest_vmaddr;
    UINT64 handoff_phys_entry = (UINT64)((INT64)entry_vmaddr +
                                         handoff_vm_slide);
    log_info(L"handoff entry_vmaddr=0x%lx host_entry=0x%lx phys_entry=0x%lx\r\n",
             entry_vmaddr, (UINT64)(UINTN)host_entry, handoff_phys_entry);
    log_info(L"handoff phys_real=0x%lx vm_base=0x%lx kslide=0x%x\r\n",
             handoff_phys_real, load_result.lowest_vmaddr, ctx.kslide);
    log_info(L"handoff kaddr=0x%lx ksize=0x%x\r\n",
             handoff_phys_real, boot_state.args->ksize);
  }
#endif
  log_info(L"stack_top = 0x%lx\r\n",      (UINT64)(UINTN)stack_top);
  log_info(L"entry     = 0x%lx\r\n",      (UINT64)(UINTN)host_entry);
#endif // VERBOSE_BOOT

#if !defined(__aarch64__) && !defined(__riscv)
  // last thing before the handoff, so no later log line lands on the boot screen
  boot_draw_logo(boot_state.args);
#endif

  /* No logging after this point */
  status = exit_boot_services_retry(&ctx, image, &boot_state);
  if (EFI_ERROR(status)) {
    log_error(L"ExitBootServices failed: %r\r\n", status);
    return status;
  }

  serial_mark((CONST CHAR8 *)"exit_boot_services returned");

#if defined(PD_ARCH_X86)
  /* Copy staged kernel image to its SLID physical base (0x100000 + kslide).
   * boot.efi physically relocates the whole image: each segment goes to
   * (vmaddr + kslide) & 0x3FFFFFFF, and the local relocs (base = __HIB) patch
   * every absolute reference by += kslide -- including pstart's own immediates
   * (mov cr3, 0x106000 -> 0x506000; ljmp 0x10105c -> 0x50105c) so the
   * bootstrap runs correctly at the slid physical location.
   * XNU's i386_vm_init derives vm_kernel_slide = kaddr - 0x100000 and panics
   * ("inconsistent slide") unless it equals boot_args->kslide, so kaddr MUST
   * be the slid base too.
   * OVMF's memory (0x800000-0x1780000) is now reclaimed as conventional. */
  UINT64 phys_real  = 0x100000ULL + ctx.kslide;

  /* kaddr = slid physical base so vm_kernel_slide == kslide; entry runs at
   * the slid low-identity physical (pstart immediates were relocated). */
  UINT64 vm_base = load_result.lowest_vmaddr;
  INT64  vm_slide = (INT64)phys_real - (INT64)vm_base;

  VOID *phys_entry = (VOID *)(UINTN)((INT64)entry_vmaddr + vm_slide);

  boot_state.args->kaddr = (UINT32)phys_real;
  boot_state.args->kslide = ctx.kslide;
#else
  /*
   * Arm64 doesn't need any of that: the kernel is already sitting exactly
   * where we staged it (ctx.kernel_region_base), and host_entry (computed
   * earlier via macho_compute_host_entry, before this function ever
   * touched phys_entry) is already the correct physical entry address in
   * that same staging region.
   */
  UINT64 vm_base = load_result.lowest_vmaddr;
  UINT64 phys_real = ctx.kernel_region_base;
  VOID *phys_entry = host_entry;
#endif

  /*
   * Kernel collection (KC) support. kc-builder converts the kernel's own
   * mach_header_64 in place into the top-level MH_FILESET header (appending
   * LC_FILESET_ENTRY/LC_DYLD_CHAINED_FIXUPS onto its existing load command
   * list, reusing its existing LC_SEGMENT_64 entries) and sets
   * MH_DYLIB_IN_CACHE (so XNU's kernel_mach_header_is_in_fileset() is true).
   * kc_mh is therefore the exact same object as the kernel's own header,
   * which lives at the start of whichever segment has fileoff==0 (__TEXT --
   * NOT necessarily the lowest-vmaddr segment: here __HIB sits at a lower
   * vmaddr than __TEXT despite coming later in the file, so phys_real/
   * vm_base, which track the lowest vmaddr, point at the wrong segment for
   * this). An earlier revision appended a synthetic "__KCHDR" wrapper
   * elsewhere, which broke kernel_collection_slide()'s internal slide
   * computation: see kc-tools/src/fileset.c's top-of-file comment for the
   * full story. XNU rebases the KC in i386_init IFF boot_args->KC_hdrs_vaddr
   * is set (Version>=2, Revision>=1 already set). We must NOT run our own
   * KASLR reloc pass for a KC (already skipped at kslide==0).
   */
  if (image_info.header->flags & 0x80000000u /* MH_DYLIB_IN_CACHE */) {
    for (UINT32 si = 0; si < load_result.segment_count; si++) {
      if (load_result.segments[si].fileoff == 0) {
        /* Same uniform vmaddr->physical delta the loader used for every
         * segment (phys_real is anchored to vm_base = lowest vmaddr). */
        boot_state.args->KC_hdrs_vaddr =
            phys_real + (load_result.segments[si].vmaddr - vm_base);
        break;
      }
    }
    /* NO log_info here: this runs post-ExitBootServices, ConOut is dead. */
  }

  /* Relocate the EFI memory map to mm_fixed and coalesce adjacent
   * same-type descriptors to keep entry count within XNU's zone limit. */
  {
    UINT8 *src = (UINT8 *)(UINTN)boot_state.args->MemoryMap;
    UINT8 *dst = (UINT8 *)(UINTN)mm_fixed;
    UINTN sz = (UINTN)boot_state.args->MemoryMapSize;
    UINTN dsz = (UINTN)boot_state.args->MemoryMapDescriptorSize;

    for (UINTN wi = 0; wi < (sz + 7) / 8; wi++)
      ((UINT64 *)dst)[wi] = ((UINT64 *)src)[wi];

    UINTN n = sz / dsz;
    for (UINTN i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(dst + i * dsz);
      if (d->Type == EfiLoaderCode     ||
          d->Type == EfiLoaderData     ||
          d->Type == EfiBootServicesCode ||
          d->Type == EfiBootServicesData)
        d->Type = EfiConventionalMemory;
    }

    serial_trace((CONST CHAR8 *)"mmap src   ", (UINT64)(UINTN)src);
    serial_trace((CONST CHAR8 *)"mmap dst   ", (UINT64)mm_fixed);
    serial_trace((CONST CHAR8 *)"mmap bytes ", (UINT64)sz);
    serial_trace((CONST CHAR8 *)"mmap dsz   ", (UINT64)dsz);
    serial_trace((CONST CHAR8 *)"mmap in    ", (UINT64)n);

    /* The relocated map must fit the 16 pages reserved at XNU_MEMMAP_PHYS. */
    if (sz > 16 * EFI_PAGE_SIZE) {
      serial_trace((CONST CHAR8 *)"FATAL mmap exceeds reserved bytes ",
                   (UINT64)(16 * EFI_PAGE_SIZE));
      for (;;) { }
    }

    UINTN out = 0;
    for (UINTN i = 0; i < n; i++) {
      EFI_MEMORY_DESCRIPTOR *cur  = (EFI_MEMORY_DESCRIPTOR *)(dst + i * dsz);
      /* `out` is the next free slot, so the previously emitted descriptor is
       * at out-1. Using `out` here made prev and cur the same pointer on every
       * iteration (out stayed equal to i precisely because nothing ever
       * merged), so every comparison was a descriptor against itself and the
       * coalescing pass silently did nothing. */
      EFI_MEMORY_DESCRIPTOR *prev =
          out > 0 ? (EFI_MEMORY_DESCRIPTOR *)(dst + (out - 1) * dsz) : NULL;
      if (prev != NULL &&
          cur->Type      == prev->Type &&
          cur->Attribute == prev->Attribute &&
          cur->PhysicalStart ==
              prev->PhysicalStart + (prev->NumberOfPages << EFI_PAGE_SHIFT)) {
        prev->NumberOfPages += cur->NumberOfPages;
      } else {
        if (out != i) {
          EFI_MEMORY_DESCRIPTOR *slot = (EFI_MEMORY_DESCRIPTOR *)(dst + out * dsz);
          for (UINTN b = 0; b < dsz; b++)
            ((UINT8 *)slot)[b] = ((UINT8 *)cur)[b];
        }
        out++;
      }
    }

    serial_trace((CONST CHAR8 *)"mmap out   ", (UINT64)out);

    if (out > 128)
      serial_trace((CONST CHAR8 *)"WARNING descriptors exceed XNU limit 128, out=",
                   (UINT64)out);

    boot_state.args->MemoryMap     = (UINT32)mm_fixed;
    boot_state.args->MemoryMapSize = (UINT32)(out * dsz);
  }

  serial_trace((CONST CHAR8 *)"boot_args  ", (UINT64)(UINTN)boot_state.args);
  serial_trace((CONST CHAR8 *)"entry      ", (UINT64)(UINTN)phys_entry);
  serial_trace((CONST CHAR8 *)"stack_top  ", (UINT64)(UINTN)stack_top);

  g_jump_entry = phys_entry;
#if defined(__aarch64__)
  g_jump_args  = (UINT64)(UINTN)arm64_args;
#else
  g_jump_args  = (UINT64)(UINTN)boot_state.args;
#endif
  g_jump_stack = stack_top;

#if defined(PD_ARCH_X86)
  /* The destination is fixed at 0x100000 while the boot-info block sits at
   * XNU_BOOTINFO_BASE, so a large enough kernel silently overwrites boot_args,
   * the device tree and the relocated memory map - and the only symptom is an
   * unexplained early hang. Nothing else checks this. */
  if (phys_real + load_result.image_size > XNU_BOOTINFO_BASE) {
    serial_trace((CONST CHAR8 *)"FATAL kernel overruns bootinfo at ",
                 XNU_BOOTINFO_BASE);
    for (;;) { }
  }

  g_copy_src   = ctx.kernel_region_base;
  g_copy_dst   = phys_real;
  g_copy_bytes = load_result.image_size;

  {
    UINT64 probe = (UINT64)(UINTN)&phys_real;
    serial_trace((CONST CHAR8 *)"firmware sp", probe);
    if (probe >= phys_real && probe < phys_real + load_result.image_size)
      serial_mark((CONST CHAR8 *)"NOTE firmware stack is inside the copy destination, switching stacks");
  }

  /* Do the copy from a stack we own, clear of the destination. */
  pd_call_on_stack(stack_top, finish_boot_and_jump, NULL);
#else
  /* Kernel image with its trust-cache page, boot-info block, and the ramdisk */
  arm64_clean_to_poc(ctx.kernel_region_base - XNU_BOOTINFO_ALIGN,
                     ctx.kernel_region_end - (ctx.kernel_region_base - XNU_BOOTINFO_ALIGN));
  arm64_clean_to_poc(XNU_BOOTINFO_BASE, XNU_BOOTINFO_END - XNU_BOOTINFO_BASE);
  arm64_clean_to_poc(ctx.ramdisk_phys, ctx.ramdisk_size);
  __asm__ volatile("ic iallu; dsb ish; isb" : : : "memory");
  serial_mark((CONST CHAR8 *)"jumping to kernel");
  jump_to_xnu(g_jump_entry, g_jump_args, g_jump_stack);
#endif
}
