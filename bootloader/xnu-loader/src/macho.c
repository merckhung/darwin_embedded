#include "macho.h"
#include "console.h"

#define MACHO_NAME_MAX 16

static UINT64 align_up(UINT64 value, UINT64 align) {
  return (value + (align - 1)) & ~(align - 1);
}

static VOID macho_copy_segname(CHAR16 *out16, const CHAR8 *in8) {
  UINTN i;
  for (i = 0; i < MACHO_NAME_MAX && in8[i] != '\0'; i++)
    out16[i] = (CHAR16)in8[i];
  for (; i < MACHO_NAME_MAX; i++)
    out16[i] = L'\0';
  out16[MACHO_NAME_MAX] = L'\0';
}

static VOID macho_copy_segname8(CHAR8 *out8, const CHAR8 *in8) {
  UINTN i;
  for (i = 0; i < MACHO_NAME_MAX && in8[i] != '\0'; i++)
    out8[i] = in8[i];
  for (; i < MACHO_NAME_MAX; i++)
    out8[i] = '\0';
  out8[MACHO_NAME_MAX] = '\0';
}

static VOID macho_copy_name16(CHAR16 *out16, const CHAR8 *in8) {
  macho_copy_segname(out16, in8);
}

static BOOLEAN macho_name16_equal(const CHAR8 *a, const CHAR8 *b) {
  UINTN i;
  for (i = 0; i < MACHO_NAME_MAX; i++) {
    if (a[i] != b[i])
      return FALSE;
    if (a[i] == '\0' && b[i] == '\0')
      return TRUE;
  }
  return a[MACHO_NAME_MAX - 1] == b[MACHO_NAME_MAX - 1];
}

static EFI_STATUS macho_validate_load_command(
    MachoImage *image,
    UINT8 *p,
    macho_load_command **out_lc)
{
  macho_load_command *lc;

  if (!image || !p || !out_lc)
    return EFI_INVALID_PARAMETER;

  UINT64 offset = (UINT64)(p - (UINT8 *)image->data);
  if (offset + sizeof(macho_load_command) > image->size)
    return EFI_COMPROMISED_DATA;

  lc = (macho_load_command *)p;

  if (lc->cmdsize == 0)
    return EFI_COMPROMISED_DATA;

  if (offset + lc->cmdsize > image->size)
    return EFI_COMPROMISED_DATA;

  *out_lc = lc;
  return EFI_SUCCESS;
}

static EFI_STATUS macho_compute_vm_range(
    MachoImage *image,
    UINT64 *out_lowest,
    UINT64 *out_highest)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;
  BOOLEAN found;
  UINT64 lowest;
  UINT64 highest;

  if (!image || !image->header || !out_lowest || !out_highest)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);
  found = FALSE;
  lowest = 0;
  highest = 0;

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      UINT64 seg_end;

      if (seg->vmsize == 0) {
        p += lc->cmdsize;
        continue;
      }

      seg_end = seg->vmaddr + seg->vmsize;
      if (seg_end < seg->vmaddr)
        return EFI_COMPROMISED_DATA;

      if (!found) {
        lowest = seg->vmaddr;
        highest = seg_end;
        found = TRUE;
      } else {
        if (seg->vmaddr < lowest)
          lowest = seg->vmaddr;
        if (seg_end > highest)
          highest = seg_end;
      }
    }

    p += lc->cmdsize;
  }

  if (!found)
    return EFI_NOT_FOUND;

  *out_lowest = lowest;
  *out_highest = highest;
  return EFI_SUCCESS;
}

EFI_STATUS macho_compute_vm_range_pub(
    MachoImage *image,
    UINT64     *out_lowest,
    UINT64     *out_highest)
{
  return macho_compute_vm_range(image, out_lowest, out_highest);
}

VOID macho_unload_contiguous(
    AppContext       *ctx,
    MachoLoadResult  *result)
{
  if (!ctx || !result || result->image_size == 0)
    return;

  UINTN pages = (UINTN)((result->image_size + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);

  uefi_call_wrapper(
      ctx->bs->FreePages, 2,
      result->host_base,
      pages);

  SetMem(result, sizeof(*result), 0);
}

EFI_STATUS macho_parse(VOID *data, UINTN size, MachoImage *out_image) {
  macho_header_64 *hdr;

  if (!data || !out_image || size < sizeof(macho_header_64))
    return EFI_INVALID_PARAMETER;

  hdr = (macho_header_64 *)data;
  if (hdr->magic != MH_MAGIC_64)
    return EFI_LOAD_ERROR;

  out_image->data = data;
  out_image->size = size;
  out_image->header = hdr;
  return EFI_SUCCESS;
}

EFI_STATUS macho_dump(MachoImage *image) {
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;

  log_info(L"Mach-O 64-bit\r\n");
  log_info(L"  ncmds: %u\r\n", hdr->ncmds);
  log_info(L"  sizeofcmds: %u\r\n", hdr->sizeofcmds);
  log_info(L"  flags: 0x%x\r\n", hdr->flags);

  p = (UINT8 *)(hdr + 1);
  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      CHAR16 name[17];

      macho_copy_segname(name, seg->segname);

      log_info(
          L"segment %-16s vm=0x%lx vmsz=0x%lx fileoff=0x%lx filesz=0x%lx\r\n",
          name,
          seg->vmaddr,
          seg->vmsize,
          seg->fileoff,
          seg->filesize);
    }

    p += lc->cmdsize;
  }

  return EFI_SUCCESS;
}

EFI_STATUS macho_load_segments(
    AppContext      *ctx,
    MachoImage      *image,
    MachoLoadResult *out_result)
{
  macho_header_64    *hdr;
  macho_load_command *lc;
  UINT8              *p;
  UINT32              i;
  EFI_STATUS          status;

  if (!ctx || !image || !image->header || !out_result)
    return EFI_INVALID_PARAMETER;

  SetMem(out_result, sizeof(*out_result), 0);
  out_result->lowest_vmaddr  = 0xFFFFFFFFFFFFFFFFULL;
  out_result->highest_vmaddr = 0;

  hdr = image->header;
  p   = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      goto fail;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      CHAR16 name16[17];

      /* Skip __PAGEZERO and zero-size segments */
      if (seg->vmsize == 0 ||
          (seg->vmaddr == 0 && seg->filesize == 0)) {
        p += lc->cmdsize;
        continue;
      }

      if (out_result->segment_count >= MACHO_MAX_SEGMENTS) {
        status = EFI_BUFFER_TOO_SMALL;
        goto fail;
      }

      /* Validate file data bounds */
      if (seg->fileoff + seg->filesize > image->size) {
        status = EFI_COMPROMISED_DATA;
        goto fail;
      }

      UINTN pages =
          (UINTN)((seg->vmsize + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT);

      /*
       * Stage into any available RAM. The final physical target (low 32 bits
       * of vmaddr) may be occupied by BootServicesData pre-ExitBootServices
       * on QEMU/OVMF. We copy to the target after EBS in efi_main.
       */
      EFI_PHYSICAL_ADDRESS staging = 0;
      status = uefi_call_wrapper(
          ctx->bs->AllocatePages, 4,
          AllocateAnyPages,
          EfiLoaderData,
          pages,
          &staging);

      if (EFI_ERROR(status)) {
        macho_copy_segname(name16, seg->segname);
        log_error(
            L"segment %s: AllocateAnyPages(%lu pages) failed: %r\r\n",
            name16, (UINT64)pages, status);
        goto fail;
      }

      UINT8 *dst = (UINT8 *)(UINTN)staging;

      /* Copy file data, zero BSS tail */
      CopyMem(dst, (UINT8 *)image->data + seg->fileoff, seg->filesize);
      if (seg->vmsize > seg->filesize)
        SetMem(dst + seg->filesize, seg->vmsize - seg->filesize, 0);

      MachoLoadedSegment *loaded =
          &out_result->segments[out_result->segment_count++];

      macho_copy_segname8(loaded->name, seg->segname);
      loaded->vmaddr      = seg->vmaddr;
      loaded->vmsize      = seg->vmsize;
      loaded->fileoff     = seg->fileoff;
      loaded->filesize    = seg->filesize;
      loaded->host_addr   = dst;
      loaded->initprot    = seg->initprot;
      loaded->page_count  = pages;
      loaded->phys_target = (UINT32)seg->vmaddr;

      if (seg->vmaddr < out_result->lowest_vmaddr)
        out_result->lowest_vmaddr = seg->vmaddr;
      if (seg->vmaddr + seg->vmsize > out_result->highest_vmaddr)
        out_result->highest_vmaddr = seg->vmaddr + seg->vmsize;

      macho_copy_segname(name16, seg->segname);
      log_info(
          L"staged  %-16s vm=0x%lx staging=0x%lx target=0x%x pages=%lu\r\n",
          name16,
          (UINT64)seg->vmaddr,
          (UINT64)staging,
          loaded->phys_target,
          (UINT64)pages);
    }

    p += lc->cmdsize;
  }

  if (out_result->segment_count == 0) {
    status = EFI_NOT_FOUND;
    goto fail;
  }

  /*
   * host_base points to the staging buffer of the lowest segment.
   * macho_compute_host_address still works correctly against staging
   * addresses for the pre-EBS entry/section lookups in efi_main.
   */
  out_result->host_base  = (EFI_PHYSICAL_ADDRESS)(UINTN)
                            out_result->segments[0].host_addr;
  out_result->image_size = out_result->highest_vmaddr - out_result->lowest_vmaddr;
  return EFI_SUCCESS;

fail:
  macho_unload_segments(ctx, out_result);
  return status;
}

VOID macho_unload_segments(
    AppContext       *ctx,
    MachoLoadResult  *result)
{
  if (!ctx || !result)
    return;

  for (UINTN i = 0; i < result->segment_count; i++) {
    MachoLoadedSegment *seg = &result->segments[i];
    if (seg->host_addr && seg->page_count) {
      uefi_call_wrapper(
          ctx->bs->FreePages, 2,
          (EFI_PHYSICAL_ADDRESS)(UINTN)seg->host_addr,
          seg->page_count);
    }
  }

  SetMem(result, sizeof(*result), 0);
}

EFI_STATUS macho_load_segments_contiguous(
    AppContext           *ctx,
    MachoImage           *image,
    EFI_PHYSICAL_ADDRESS  phys_base,
    MachoLoadResult      *out_result)
{
  EFI_STATUS status;
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!ctx || !image || !image->header || !out_result)
    return EFI_INVALID_PARAMETER;

  SetMem(out_result, sizeof(*out_result), 0);
  out_result->lowest_vmaddr = 0xFFFFFFFFFFFFFFFFULL;
  out_result->highest_vmaddr = 0;

  /* compute vm range so we know total span */
  hdr = image->header;
  p = (UINT8 *)(hdr + 1);
  for (i = 0; i < hdr->ncmds; i++) {
    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status)) return status;
    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      if (seg->vmsize == 0 || (seg->vmaddr == 0 && seg->filesize == 0)) {
        p += lc->cmdsize; continue;
      }
      if (seg->vmaddr < out_result->lowest_vmaddr)
        out_result->lowest_vmaddr = seg->vmaddr;
      if (seg->vmaddr + seg->vmsize > out_result->highest_vmaddr)
        out_result->highest_vmaddr = seg->vmaddr + seg->vmsize;
    }
    p += lc->cmdsize;
  }

  if (out_result->lowest_vmaddr == 0xFFFFFFFFFFFFFFFFULL)
    return EFI_NOT_FOUND;

  UINT64 lowest = out_result->lowest_vmaddr;
  UINT64 span = out_result->highest_vmaddr - lowest;

  /* Verify the image fits within the pre-reserved kernel region */
  if (ctx->kernel_region_base == 0) {
    log_error(L"kernel region not reserved, call AllocKernelMemRegion first\r\n");
    return EFI_NOT_READY;
  }
  if (phys_base < ctx->kernel_region_base ||
      phys_base + span > ctx->kernel_region_end) {
    log_error(
        L"image [0x%lx, 0x%lx) overflows kernel region [0x%lx, 0x%lx)\r\n",
        phys_base, phys_base + span,
        ctx->kernel_region_base, ctx->kernel_region_end);
    return EFI_BUFFER_TOO_SMALL;
  }

  /*
   * Memory is already allocated and zeroed by AllocKernelMemRegion.
   * No AllocatePages call needed; just use it directly.
   */
  out_result->host_base  = phys_base;
  out_result->image_size = span;

  p = (UINT8 *)(hdr + 1);
  for (i = 0; i < hdr->ncmds; i++) {
    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status)) return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      CHAR16 name16[17];

      if (seg->vmsize == 0 || (seg->vmaddr == 0 && seg->filesize == 0)) {
        p += lc->cmdsize; continue;
      }
      if (out_result->segment_count >= MACHO_MAX_SEGMENTS)
        return EFI_BUFFER_TOO_SMALL;
      if (seg->fileoff + seg->filesize > image->size)
        return EFI_COMPROMISED_DATA;

      UINT64 offset = seg->vmaddr - lowest;
      UINT8 *dst    = (UINT8 *)(UINTN)(phys_base + offset);

      CopyMem(dst, (UINT8 *)image->data + seg->fileoff, seg->filesize);
      /* BSS tail already zeroed by AllocKernelMemRegion's SetMem */

      MachoLoadedSegment *loaded = &out_result->segments[out_result->segment_count++];
      macho_copy_segname8(loaded->name, seg->segname);
      loaded->vmaddr      = seg->vmaddr;
      loaded->vmsize      = seg->vmsize;
      loaded->fileoff     = seg->fileoff;
      loaded->filesize    = seg->filesize;
      loaded->host_addr   = dst;
      loaded->initprot    = seg->initprot;
      loaded->page_count  = 0;   /* owned by kernel region allocation */
      loaded->phys_target = (UINT32)(phys_base + offset);

      macho_copy_segname(name16, seg->segname);
      log_info(L"loaded  %-16s vm=0x%lx phys=0x%lx pages=%lu\r\n",
               name16, seg->vmaddr,
               (UINT64)(UINTN)dst,
               (UINT64)((seg->vmsize + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT));
    }
    p += lc->cmdsize;
  }

  return EFI_SUCCESS;
}

EFI_STATUS macho_find_entry_vmaddr(
    MachoImage *image,
    UINT64 *out_entry_vmaddr) {
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_entry_vmaddr)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_UNIXTHREAD) {
      UINT8 *q = p + sizeof(macho_thread_command);
      UINT8 *end = p + lc->cmdsize;

      while (q + sizeof(UINT32) * 2 <= end) {
        UINT32 flavor = *(UINT32 *)q;
        UINT32 count  = *(UINT32 *)(q + sizeof(UINT32));
        UINT8 *state  = q + sizeof(UINT32) * 2;
        UINTN state_size = (UINTN)count * sizeof(UINT32);

        if (state + state_size > end)
          return EFI_COMPROMISED_DATA;

        if (flavor == X86_THREAD_STATE64) {
          if (state_size < sizeof(x86_thread_state64))
            return EFI_COMPROMISED_DATA;

          x86_thread_state64 *ts = (x86_thread_state64 *)state;
          *out_entry_vmaddr = ts->rip;
          return EFI_SUCCESS;
        }

        // flavor 1 means something else on the other architectures
        if (hdr->cputype == CPU_TYPE_RISCV64 && flavor == RISCV_THREAD_STATE64) {
          if (state_size < sizeof(riscv_thread_state64))
            return EFI_COMPROMISED_DATA;

          riscv_thread_state64 *ts = (riscv_thread_state64 *)state;
          *out_entry_vmaddr = ts->pc;
          return EFI_SUCCESS;
        }

        if (flavor == ARM_THREAD_STATE64) {
          if (state_size < sizeof(arm_thread_state64))
            return EFI_COMPROMISED_DATA;

          arm_thread_state64 *ts = (arm_thread_state64 *)state;
          *out_entry_vmaddr = ts->pc;
          return EFI_SUCCESS;
        }

        q = state + state_size;
      }

      return EFI_NOT_FOUND;
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_compute_host_entry(
    MachoLoadResult *load_result,
    UINT64 entry_vmaddr,
    VOID **out_host_entry)
{
  return macho_compute_host_address(load_result, entry_vmaddr, out_host_entry);
}

EFI_STATUS macho_find_segment_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_seg)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;

      if (seg->vmsize != 0 &&
          vmaddr >= seg->vmaddr &&
          vmaddr < seg->vmaddr + seg->vmsize) {
        *out_seg = seg;
        return EFI_SUCCESS;
      }
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_find_section_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !out_seg || !out_sec)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      macho_section_64 *sec;
      UINT32 j;

      if (seg->vmsize == 0) {
        p += lc->cmdsize;
        continue;
      }

      if (!(vmaddr >= seg->vmaddr && vmaddr < seg->vmaddr + seg->vmsize)) {
        p += lc->cmdsize;
        continue;
      }

      sec = (macho_section_64 *)(seg + 1);
      for (j = 0; j < seg->nsects; j++) {
        if (vmaddr >= sec[j].addr && vmaddr < sec[j].addr + sec[j].size) {
          *out_seg = seg;
          *out_sec = &sec[j];
          return EFI_SUCCESS;
        }
      }

      *out_seg = seg;
      *out_sec = NULL;
      return EFI_SUCCESS;
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

VOID macho_log_entry_context(
    MachoImage *image,
    UINT64 entry_vmaddr)
{
  EFI_STATUS status;
  macho_segment_command_64 *seg = NULL;
  macho_section_64 *sec = NULL;
  CHAR16 segname[17];
  CHAR16 sectname[17];

  status = macho_find_section_for_vmaddr(image, entry_vmaddr, &seg, &sec);
  if (EFI_ERROR(status)) {
    log_error(L"failed to locate entry segment/section: %r\r\n", status);
    return;
  }

  macho_copy_name16(segname, seg->segname);

  if (sec) {
    macho_copy_name16(sectname, sec->sectname);
    log_info(
        L"entry is in segment %s, section %s, seg_off=0x%lx sec_off=0x%lx\r\n",
        segname,
        sectname,
        entry_vmaddr - seg->vmaddr,
        entry_vmaddr - sec->addr);
  } else {
    log_info(
        L"entry is in segment %s, no section match, seg_off=0x%lx\r\n",
        segname,
        entry_vmaddr - seg->vmaddr);
  }
}

VOID macho_dump_entry_bytes(
    VOID *host_entry,
    UINTN count)
{
  UINT8 *p;
  UINTN i;

  if (!host_entry || count == 0)
    return;

  p = (UINT8 *)host_entry;

  log_info(L"entry bytes:");
  for (i = 0; i < count; i++) {
    if ((i % 16) == 0)
      log_info(L"\r\n  ");
    log_info(L"%02x ", (UINTN)p[i]);
  }
  log_info(L"\r\n");
}

EFI_STATUS macho_find_section_by_name(
    MachoImage *image,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec)
{
  macho_header_64 *hdr;
  macho_load_command *lc;
  UINT8 *p;
  UINT32 i;

  if (!image || !image->header || !segname || !sectname || !out_seg || !out_sec)
    return EFI_INVALID_PARAMETER;

  hdr = image->header;
  p = (UINT8 *)(hdr + 1);

  for (i = 0; i < hdr->ncmds; i++) {
    EFI_STATUS status;

    status = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(status))
      return status;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      macho_section_64 *sec = (macho_section_64 *)(seg + 1);
      UINT32 j;

      if (!macho_name16_equal(seg->segname, segname)) {
        p += lc->cmdsize;
        continue;
      }

      for (j = 0; j < seg->nsects; j++) {
        if (macho_name16_equal(sec[j].sectname, sectname)) {
          *out_seg = seg;
          *out_sec = &sec[j];
          return EFI_SUCCESS;
        }
      }
    }

    p += lc->cmdsize;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS macho_compute_host_address(
    MachoLoadResult *load_result,
    UINT64           vmaddr,
    VOID           **out_host_addr)
{
  if (!load_result || !out_host_addr)
    return EFI_INVALID_PARAMETER;

  for (UINTN i = 0; i < load_result->segment_count; i++) {
    MachoLoadedSegment *seg = &load_result->segments[i];
    if (vmaddr >= seg->vmaddr && vmaddr < seg->vmaddr + seg->vmsize) {
      *out_host_addr =
          (VOID *)((UINT8 *)seg->host_addr + (vmaddr - seg->vmaddr));
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;  /* not inside any loaded segment */
}

EFI_STATUS macho_get_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    UINT64 *out_vmaddr,
    UINT64 *out_size,
    VOID **out_host_addr)
{
  EFI_STATUS status;
  macho_segment_command_64 *seg = NULL;
  macho_section_64 *sec = NULL;
  VOID *host_addr = NULL;

  if (!image || !load_result || !segname || !sectname ||
      !out_vmaddr || !out_size || !out_host_addr)
    return EFI_INVALID_PARAMETER;

  status = macho_find_section_by_name(image, segname, sectname, &seg, &sec);
  if (EFI_ERROR(status))
    return status;

  if (!sec)
    return EFI_NOT_FOUND;

  status = macho_compute_host_address(load_result, sec->addr, &host_addr);
  if (EFI_ERROR(status))
    return status;

  *out_vmaddr = sec->addr;
  *out_size = sec->size;
  *out_host_addr = host_addr;
  return EFI_SUCCESS;
}

EFI_STATUS macho_apply_kaslr_slide(
    MachoImage      *image,
    MachoLoadResult *load_result,
    UINT32           kslide)
{
  if (!image || !image->header || !load_result || kslide == 0)
    return EFI_SUCCESS;

  macho_header_64    *hdr = image->header;
  UINT8              *p   = (UINT8 *)(hdr + 1);
  macho_dysymtab_command *dysymtab = NULL;

  for (UINT32 i = 0; i < hdr->ncmds; i++) {
    macho_load_command *lc;
    EFI_STATUS s = macho_validate_load_command(image, p, &lc);
    if (EFI_ERROR(s)) return s;
    if (lc->cmd == LC_DYSYMTAB) {
      dysymtab = (macho_dysymtab_command *)lc;
      break;
    }
    p += lc->cmdsize;
  }

  if (!dysymtab || dysymtab->nlocrel == 0) {
    log_info(L"KASLR: no local relocs, skipping fixup\r\n");
    goto slide_targets;
  }

  /* Validate file bounds for the reloc table */
  UINT64 rel_end = (UINT64)dysymtab->locreloff +
                   (UINT64)dysymtab->nlocrel * sizeof(macho_relocation_info);
  if (rel_end > image->size) {
    log_info(L"KASLR: locrel table out of bounds\r\n");
    return EFI_COMPROMISED_DATA;
  }

  macho_relocation_info *relocs =
      (macho_relocation_info *)((UINT8 *)image->data + dysymtab->locreloff);

  /* r_address in the LC_DYSYMTAB local relocs is an offset from the FIRST
   * WRITABLE segment (__DATA), matching boot.efi's
   *   *(g_first_writable_seg_phys + r_address) += g_kslide
   * Verified against the kernel image: reloc[0] has r_address 0 and __DATA[0]
   * holds a kernel VA pointer (0xffffff80013...), whereas __HIB[0] is code.
   * pstart's own __HIB bootstrap immediates are covered by the signed
   * (negative r_address) entries in this same table once r_address is treated
   * as INT32, so no separate fixup pass is needed. */
  UINT8 *reloc_base = (UINT8 *)(UINTN)load_result->host_base; /* fallback */
  for (UINTN si = 0; si < load_result->segment_count; si++) {
    MachoLoadedSegment *seg = &load_result->segments[si];
    if ((seg->initprot & 2) && seg->vmsize > 0) {
      reloc_base = (UINT8 *)seg->host_addr;
      break;
    }
  }

  UINT8 *img_lo = (UINT8 *)(UINTN)load_result->host_base;
  UINT8 *img_hi = img_lo + (UINTN)load_result->image_size;
  UINT32 patched = 0, skipped = 0;

  for (UINT32 i = 0; i < dysymtab->nlocrel; i++) {
    macho_relocation_info *rel = &relocs[i];

    /* Skip pc-relative and external relocs; only handle 32- and 64-bit abs */
    if (rel->r_pcrel || rel->r_extern ||
        (rel->r_length != 3 && rel->r_length != 2)) {
      skipped++;
      continue;
    }

    /* r_address is a SIGNED offset from the first writable segment (__DATA):
     * boot.efi computes the fixup site as *((int *)&reloc)[0] added to
     * g_first_writable_seg_phys, so NEGATIVE r_address reaches back below
     * __DATA into __TEXT/__HIB (e.g. __HIB's absolute descriptor pointers used
     * by doublemap_init).  Treating it as unsigned turns those into huge
     * offsets that fall out of bounds and get skipped -> __HIB pointers never
     * slid -> poison-pointer fault in the double map. */
    UINT8 *host_ptr = reloc_base + (INT64)(INT32)rel->r_address;

    /* Bounds check the fixup site against the loaded image span */
    UINTN patch_size = (rel->r_length == 3) ? 8 : 4;
    if (host_ptr < img_lo || host_ptr + patch_size > img_hi) {
      skipped++;
      continue;
    }

    if (rel->r_length == 3)
      *(UINT64 *)host_ptr += (UINT64)kslide;
    else
      *(UINT32 *)host_ptr += kslide;

    patched++;
  }

  log_info(L"KASLR: applied slide 0x%x to %u relocs (%u skipped)\r\n",
           kslide, patched, skipped);

slide_targets:
  /* Shift every segment's physical target address by kslide */
  for (UINTN i = 0; i < load_result->segment_count; i++)
    load_result->segments[i].phys_target += kslide;

  return EFI_SUCCESS;
}

EFI_STATUS macho_patch_header_slide(
    MachoImage      *image,
    MachoLoadResult *load_result,
    UINT32           kslide)
{
  if (!image || !load_result || kslide == 0)
    return EFI_SUCCESS;

  /* Locate the __TEXT segment in staging; it holds _mh_execute_header at
   * fileoff 0, i.e. at the very start of its staged bytes. */
  UINT8 *texthost = NULL;
  UINT64 textsize = 0;
  for (UINTN i = 0; i < load_result->segment_count; i++) {
    MachoLoadedSegment *seg = &load_result->segments[i];
    if (seg->fileoff == 0 && seg->filesize > 0) {
      texthost = (UINT8 *)seg->host_addr;
      textsize = seg->filesize;
      break;
    }
  }
  if (!texthost) {
    log_error(L"KASLR: __TEXT (fileoff 0) not found; cannot patch header\r\n");
    return EFI_NOT_FOUND;
  }

  macho_header_64 *hdr = (macho_header_64 *)texthost;
  if (hdr->magic != MH_MAGIC_64) {
    log_error(L"KASLR: embedded header magic mismatch 0x%x\r\n", hdr->magic);
    return EFI_COMPROMISED_DATA;
  }

  UINT8 *p   = texthost + sizeof(macho_header_64);
  UINT8 *end = texthost + textsize;
  UINT32 segs = 0, sects = 0;

  for (UINT32 i = 0; i < hdr->ncmds; i++) {
    if (p + sizeof(macho_load_command) > end) break;
    macho_load_command *lc = (macho_load_command *)p;
    if (lc->cmdsize < sizeof(macho_load_command) || p + lc->cmdsize > end)
      break;

    if (lc->cmd == LC_SEGMENT_64) {
      macho_segment_command_64 *seg = (macho_segment_command_64 *)p;
      /* Slide the segment's own vmaddr (skip __PAGEZERO-style vmaddr==0). */
      if (seg->vmaddr != 0)
        seg->vmaddr += (UINT64)kslide;
      segs++;

      macho_section_64 *sc =
          (macho_section_64 *)(p + sizeof(macho_segment_command_64));
      for (UINT32 s = 0; s < seg->nsects; s++) {
        if ((UINT8 *)(sc + 1) > end) break;
        if (sc->addr != 0)
          sc->addr += (UINT64)kslide;
        sects++;
        sc++;
      }
    }
    p += lc->cmdsize;
  }

  log_info(L"KASLR: slid embedded header (%u segs, %u sects) by +0x%x\r\n",
           segs, sects, kslide);
  return EFI_SUCCESS;
}

VOID macho_log_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname)
{
  EFI_STATUS status;
  UINT64 vmaddr = 0;
  UINT64 size = 0;
  VOID *host_addr = NULL;
  CHAR16 seg16[17];
  CHAR16 sec16[17];

  status = macho_get_section_host_info(
      image,
      load_result,
      segname,
      sectname,
      &vmaddr,
      &size,
      &host_addr);

  macho_copy_name16(seg16, (CHAR8 *)segname);
  macho_copy_name16(sec16, (CHAR8 *)sectname);

  if (EFI_ERROR(status)) {
    log_error(L"failed to find section %s,%s: %r\r\n", seg16, sec16, status);
    return;
  }

  log_info(
      L"section %s,%s vm=0x%lx host=0x%lx size=0x%lx\r\n",
      seg16,
      sec16,
      vmaddr,
      (UINT64)(UINTN)host_addr,
      size);
}