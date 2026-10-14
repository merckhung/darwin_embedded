#include "prelink.h"
#include "macho_view.h"
#include "plist.h"
#include "macho_fmt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PAGE 0x1000ULL
#define ALIGN_UP(x, a) (((uint64_t)(x) + ((uint64_t)(a) - 1)) & ~((uint64_t)(a) - 1))

static void put32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}

int prelink_assemble(const uint8_t *kernel_data, size_t kernel_size,
                     LinkedKext *kexts, size_t num_kexts,
                     uint8_t **out_data, size_t *out_size)
{
  MachoView mv;
  if (macho_view(kernel_data, kernel_size, &mv) != 0) {
    fprintf(stderr, "prelink: kernel is not a Mach-O\n");
    return -1;
  }

  // Find highest kernel VM address for kext placement.
  // Skip zero-size placeholder __PRELINK_TEXT (vmsize=0) segments.
  uint64_t kernel_vm_end = 0;
  {
    const uint8_t *lcp = kernel_data + mv.hdrsize;
    for (uint32_t i = 0; i < mv.ncmds; i++) {
      const struct load_command *lc = (const struct load_command *)lcp;
      if (macho_is_segment(&mv, lcp)) {
        MachoSeg segv; macho_seg_get(&mv, lcp, &segv);
        const MachoSeg *sc = &segv;
        if (sc->vmsize > 0) {
          uint64_t end = sc->vmaddr + sc->vmsize;
          if (end > kernel_vm_end) kernel_vm_end = end;
        }
      }
      lcp += lc->cmdsize;
    }
  }
  kernel_vm_end = ALIGN_UP(kernel_vm_end, PAGE * 64);
  fprintf(stderr, "prelink: kernel vm end = 0x%llx\n",
          (unsigned long long)kernel_vm_end);

  // VMaddrs were already assigned by the caller (main.c).
  // Compute __PRELINK_TEXT extent from the kexts' actual load_vmaddrs.
  uint64_t prelink_text_vmaddr = kexts[0].load_vmaddr;
  uint64_t kext_end = prelink_text_vmaddr;
  for (size_t i = 0; i < num_kexts; i++) {
    if (kexts[i].codeless) continue;   // no code to place
    uint64_t occupied = kexts[i].vm_extent ? kexts[i].vm_extent
                                           : kexts[i].linked_size;
    uint64_t end = kexts[i].load_vmaddr + ALIGN_UP(occupied, PAGE);
    if (end > kext_end) kext_end = end;
    fprintf(stderr, "  kext[%zu] %s: vmaddr=0x%llx size=0x%zx kmod_info=0x%llx\n",
            i, kexts[i].bundle_id,
            (unsigned long long)kexts[i].load_vmaddr,
            kexts[i].linked_size,
            (unsigned long long)kexts[i].kmod_info_addr);
  }
  uint64_t prelink_text_vmsize = kext_end - prelink_text_vmaddr;

  // Compute the file-layout values that the plist needs.
  // prelink_text_fileoff only depends on kernel_size, so it's safe here.
  size_t prelink_text_fileoff = ALIGN_UP(kernel_size, PAGE);

  // Build the plist now that prelink_text_fileoff is known.
  // XNU (bootstrap.cpp) deserializes this as OSDictionary, then reads
  // the kext array from "_PrelinkInfoDictionary" (OSKext.cpp:9805).
  // IDA's XNU loader expects the same outer-dict wrapper.
  PlistNode *info_array = plist_new_array();
  for (size_t i = 0; i < num_kexts; i++) {
    LinkedKext *k = &kexts[i];
    PlistNode *dict = NULL;
    if (k->info_plist_xml && k->info_plist_len)
      dict = plist_parse(k->info_plist_xml, k->info_plist_len);
    if (!dict || dict->type != PLIST_DICT) {
      if (dict) plist_free(dict);
      dict = plist_new_dict();
    }
    plist_dict_set(dict, "_PrelinkBundlePath",            plist_new_string(k->bundle_path));
    if (!k->codeless) {
      // Real kext: full executable placement keys.
      uint64_t file_off = prelink_text_fileoff + (k->load_vmaddr - prelink_text_vmaddr);
      plist_dict_set(dict, "_PrelinkExecutableLoadAddr",    plist_new_integer(k->load_vmaddr));
      plist_dict_set(dict, "_PrelinkExecutableFileOffset",  plist_new_integer(file_off));
      plist_dict_set(dict, "_PrelinkExecutableSize",        plist_new_integer(ALIGN_UP(k->linked_size, PAGE)));
      plist_dict_set(dict, "_PrelinkExecutableSourceAddr",  plist_new_integer(k->load_vmaddr));
      plist_dict_set(dict, "_PrelinkKmodInfo",              plist_new_integer(k->kmod_info_addr));
    }
    // Codeless kexts carry no _PrelinkExecutable* keys — XNU treats the absent
    // load address as a codeless (symbol/dependency-only) kext.
    plist_array_append(info_array, dict);
  }
  PlistNode *info_root = plist_new_dict();
  plist_dict_set(info_root, "_PrelinkInfoDictionary", info_array);
  size_t info_xml_len = 0;
  char *info_xml = plist_write_xml(info_root, &info_xml_len);
  plist_free(info_root);

  // Now that info_xml_len is known, compute the rest of the layout.
  uint64_t prelink_info_vmaddr = ALIGN_UP(kext_end, PAGE * 4);
  uint64_t prelink_info_vmsize = ALIGN_UP(info_xml_len + 1, PAGE);
  size_t prelink_info_fileoff = ALIGN_UP(prelink_text_fileoff + prelink_text_vmsize, PAGE);
  size_t total_size = prelink_info_fileoff + prelink_info_vmsize;

  // Find existing zero-size __PRELINK_TEXT, __PRELINK_DATA, __PRELINK_INFO
  // placeholders to patch in-place. If not found we append new load commands.
  uint8_t *existing_pt = NULL;  // __PRELINK_TEXT
  uint8_t *existing_pd = NULL;  // __PRELINK_DATA
  uint8_t *existing_pi = NULL;  // __PRELINK_INFO
  {
    // Use a mutable copy (will be the output buf), but scan original for now.
    const uint8_t *lcp = kernel_data + mv.hdrsize;
    for (uint32_t i = 0; i < mv.ncmds; i++) {
      const struct load_command *lc = (const struct load_command *)lcp;
      if (macho_is_segment(&mv, lcp)) {
        MachoSeg segv; macho_seg_get(&mv, lcp, &segv);
        if (!memcmp(segv.segname, "__PRELINK_TEXT", 14) && segv.vmsize == 0)
          existing_pt = (uint8_t *)lcp;
        if (!memcmp(segv.segname, "__PRELINK_DATA", 14) && segv.vmsize == 0)
          existing_pd = (uint8_t *)lcp;
        if (!memcmp(segv.segname, "__PRELINK_INFO", 14) && segv.vmsize == 0)
          existing_pi = (uint8_t *)lcp;
      }
      lcp += lc->cmdsize;
    }
  }

  // How many new LCs do we need to append?
  size_t extra_lc = 0;
  if (!existing_pt)
    extra_lc += macho_seg1_cmdsize(&mv);
  if (!existing_pd)
    extra_lc += macho_seg1_cmdsize(&mv);
  if (!existing_pi)
    extra_lc += macho_seg1_cmdsize(&mv);

  // Find where kernel segment data starts (first non-zero fileoff).
  uint64_t kernel_body_off = 0;
  {
    const uint8_t *lcp = kernel_data + mv.hdrsize;
    for (uint32_t i = 0; i < mv.ncmds; i++) {
      const struct load_command *lc = (const struct load_command *)lcp;
      if (macho_is_segment(&mv, lcp)) {
        MachoSeg segv; macho_seg_get(&mv, lcp, &segv);
        if (segv.filesize > 0 && (!kernel_body_off || segv.fileoff < kernel_body_off))
          kernel_body_off = segv.fileoff;
      }
      lcp += lc->cmdsize;
    }
  }
  if (!kernel_body_off)
    kernel_body_off = PAGE;

  if (extra_lc > 0) {
    size_t cur_hdr_end = mv.hdrsize + mv.sizeofcmds;
    if (cur_hdr_end + extra_lc > kernel_body_off) {
      fprintf(stderr, "prelink: no room for %zu bytes of new LCs "
              "(header ends at 0x%zx, body at 0x%llx)\n",
              extra_lc, cur_hdr_end, (unsigned long long)kernel_body_off);
      free(info_xml);
      return -1;
    }
  }

  uint8_t *out = calloc(1, total_size);
  if (!out) { free(info_xml); return -1; }
  memcpy(out, kernel_data, kernel_size);

  // Write kext binaries into __PRELINK_TEXT region and patch filetype.
  for (size_t i = 0; i < num_kexts; i++) {
    LinkedKext *k = &kexts[i];
    if (k->codeless || !k->linked_data) continue;   // codeless: no code
    size_t kext_file_base = prelink_text_fileoff + (k->load_vmaddr - prelink_text_vmaddr);
    if (kext_file_base + k->linked_size > total_size)
      continue;
    memcpy(out + kext_file_base, k->linked_data, k->linked_size);

    uint8_t *kmh = out + kext_file_base;
    MachoView kv;
    if (macho_view(kmh, k->linked_size, &kv) != 0 || kv.base != kmh)
      continue;

    // Fix filetype: MH_BUNDLE -> MH_KEXT_BUNDLE.
    if (macho_hdr_filetype(&kv, kmh) == MH_BUNDLE)
      macho_hdr_set_filetype(&kv, kmh, MH_KEXT_BUNDLE);

    // Clear flags that are invalid for kexts: MH_TWOLEVEL (0x4),
    // MH_NOFIXPREBINDING (0x80). Leave MH_NOUNDEFS (0x1).
    macho_hdr_set_flags(&kv, kmh, macho_hdr_flags(&kv, kmh) & ~(0x4u | 0x80u));

    // Strip load commands that are dyld-specific and invalid in MH_KEXT_BUNDLE.
    // IDA validates that kexts don't have these. Compact the LC area in-place.
    {
      uint8_t *lc_base  = kmh + kv.hdrsize;
      uint8_t *lc_read  = lc_base;
      uint8_t *lc_write = lc_base;
      uint32_t new_ncmds = 0, new_szcmds = 0;
      uint32_t kncmds = macho_hdr_ncmds(&kv, kmh);
      for (uint32_t ci = 0; ci < kncmds; ci++) {
        uint32_t cmd = *(uint32_t *)lc_read;
        uint32_t csz = *(uint32_t *)(lc_read + 4);
        int drop = (cmd == 0x22 || cmd == 0x80000022 || // LC_DYLD_INFO(_ONLY)
                    cmd == 0x0c || // LC_LOAD_DYLIB
                    cmd == 0x80000018 || // LC_LOAD_WEAK_DYLIB
                    cmd == 0x28 || // LC_LAZY_LOAD_DYLIB
                    cmd == 0x1d || // LC_CODE_SIGNATURE
                    cmd == 0x2c);  // LC_DYLD_ENVIRONMENT
        if (!drop) {
          if (lc_write != lc_read)
            memmove(lc_write, lc_read, csz);
          lc_write += csz;
          new_ncmds++;
          new_szcmds += csz;
        }
        lc_read += csz;
      }
      // Zero the vacated space.
      if (lc_write < lc_read)
        memset(lc_write, 0, (size_t)(lc_read - lc_write));
      macho_hdr_set_cmds(&kv, kmh, new_ncmds, new_szcmds);
    }
  }

  // Write __PRELINK_INFO XML.
  memcpy(out + prelink_info_fileoff, info_xml, info_xml_len);
  free(info_xml);

  // Pointer fixup: existing_pt/pd/pi point into kernel_data; rebase to out.
  ptrdiff_t rebase = (ptrdiff_t)(out - kernel_data);
  if (existing_pt)
    existing_pt += rebase;
  if (existing_pd)
    existing_pd += rebase;
  if (existing_pi)
    existing_pi += rebase;

  uint8_t *new_mh = out;
  uint32_t new_ncmds = macho_hdr_ncmds(&mv, new_mh);
  uint32_t new_szcmds = macho_hdr_sizeofcmds(&mv, new_mh);
  uint8_t *lc_end = out + mv.hdrsize + new_szcmds;

  // Emit (or patch in place) a segment + its single section, in the kernel's
  // Mach-O width.
  #define PUT_SEG(slot, sname, stn, vm, vsz, foff, fsz, mp, ip, saddr, ssz, soff, aln) \
    do { \
      if (slot) { \
        macho_emit_seg1(&mv, (slot), sname, (vm), (vsz), (foff), (fsz), (mp), (ip), \
                        stn, (saddr), (ssz), (uint32_t)(soff), (aln)); \
      } else { \
        uint32_t csz = macho_emit_seg1(&mv, lc_end, sname, (vm), (vsz), (foff), (fsz), \
                                       (mp), (ip), stn, (saddr), (ssz), \
                                       (uint32_t)(soff), (aln)); \
        lc_end += csz; \
        new_ncmds++; \
        new_szcmds += csz; \
      } \
    } while (0)

  // __PRELINK_TEXT
  PUT_SEG(existing_pt, "__PRELINK_TEXT", "__text",
          prelink_text_vmaddr, prelink_text_vmsize,
          prelink_text_fileoff, prelink_text_vmsize,
          VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE,
          prelink_text_vmaddr, prelink_text_vmsize, prelink_text_fileoff, 12);

  // __PRELINK_DATA (empty placeholder between __PRELINK_TEXT and __PRELINK_INFO)
  uint64_t pd_vmaddr = prelink_info_vmaddr - PAGE;
  PUT_SEG(existing_pd, "__PRELINK_DATA", "__data",
          pd_vmaddr, PAGE, 0, 0,
          VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
          pd_vmaddr, 0, 0, 0);

  // __PRELINK_INFO
  PUT_SEG(existing_pi, "__PRELINK_INFO", "__info",
          prelink_info_vmaddr, prelink_info_vmsize,
          prelink_info_fileoff, info_xml_len + 1,
          VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
          prelink_info_vmaddr, info_xml_len + 1, prelink_info_fileoff, 0);

  macho_hdr_set_cmds(&mv, new_mh, new_ncmds, new_szcmds);

  #undef PUT_SEG


  (void)put64;

  *out_data = out;
  *out_size = total_size;
  return 0;
}