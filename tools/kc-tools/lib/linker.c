#include "linker.h"
#include "macho_view.h"
#include "macho_fmt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static uint32_t r32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t r64(const uint8_t *p) {
  return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32);
}

static void w32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void w64(uint8_t *p, uint64_t v) {
  w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t rptr(const uint8_t *p, bool is64) {
  return is64 ? r64(p) : r32(p);
}

static void wptr(uint8_t *p, uint64_t v, bool is64) {
  if (is64)
    w64(p, v);
  else
    w32(p, (uint32_t)v);
}

static uint64_t read_uleb(const uint8_t **pp, const uint8_t *end) {
  uint64_t r = 0; int s = 0; const uint8_t *p = *pp;
  while (p < end) {
    uint8_t b = *p++;
    r |= (uint64_t)(b & 0x7f) << s;
    s += 7;
    if (!(b & 0x80))
      break;
  }
  *pp = p;
  return r;
}

static int64_t read_sleb(const uint8_t **pp, const uint8_t *end) {
  int64_t r = 0; int s = 0; const uint8_t *p = *pp; uint8_t b = 0;
  while (p < end) {
    b = *p++;
    r |= (int64_t)(b & 0x7f) << s;
    s += 7;
    if (!(b & 0x80))
      break;
  }
  if (s < 64 && (b & 0x40))
    r |= -(int64_t)(1ULL << s);
  *pp = p;
  return r;
}

static int sym_cmp(const void *a, const void *b) {
  return strcmp(((Sym *)a)->name, ((Sym *)b)->name);
}

// LC_DYLD_INFO(_ONLY) command + payload (mach-o/loader.h). Defined here so this
// shared linker is self-contained across both trees' macho_fmt.h variants.
#ifndef LC_DYLD_INFO
#define LC_DYLD_INFO 0x22
#endif

#ifndef LC_DYLD_INFO_ONLY
#define LC_DYLD_INFO_ONLY (0x22 | 0x80000000)   // 0x22 | LC_REQ_DYLD
#endif

struct dyld_info_command {
  uint32_t cmd;
  uint32_t cmdsize;
  uint32_t rebase_off;
  uint32_t rebase_size;
  uint32_t bind_off;
  uint32_t bind_size;
  uint32_t weak_bind_off;
  uint32_t weak_bind_size;
  uint32_t lazy_bind_off;
  uint32_t lazy_bind_size;
  uint32_t export_off;
  uint32_t export_size;
};

#define REBASE_OPCODE_MASK                              0xF0
#define REBASE_IMMEDIATE_MASK                           0x0F
#define REBASE_OPCODE_DONE                              0x00
#define REBASE_OPCODE_SET_TYPE_IMM                      0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB       0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB                     0x30
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED               0x40
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES               0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES              0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB           0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

#define BIND_OPCODE_MASK                                0xF0
#define BIND_IMMEDIATE_MASK                             0x0F
#define BIND_OPCODE_DONE                                0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM               0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB              0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM               0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM       0x40
#define BIND_OPCODE_SET_TYPE_IMM                        0x50
#define BIND_OPCODE_SET_ADDEND_SLEB                     0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB         0x70
#define BIND_OPCODE_ADD_ADDR_ULEB                       0x80
#define BIND_OPCODE_DO_BIND                             0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB               0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED         0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB    0xC0

#ifndef LC_FUNCTION_STARTS
#define LC_FUNCTION_STARTS 0x26
#endif
#ifndef LC_DATA_IN_CODE
#define LC_DATA_IN_CODE 0x29
#endif
#ifndef LC_DYLD_CHAINED_FIXUPS
#define LC_DYLD_CHAINED_FIXUPS (0x34 | 0x80000000)
#endif
#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE (0x33 | 0x80000000)
#endif

typedef struct { uint64_t fileoff; uint64_t vmaddr; uint64_t filesize; } SegInfo;

static void symtab_add(SymTable *st, const char *name, uint64_t addr) {
  if (st->count == st->cap) {
    st->cap = st->cap ? st->cap * 2 : 4096;
    st->syms = realloc(st->syms, st->cap * sizeof(Sym));
  }
  st->syms[st->count].name = strdup(name);
  st->syms[st->count].addr = addr;
  st->count++;
}

static const uint8_t *find_slice(const uint8_t *data, size_t size, size_t *out_sz) {
  MachoView mv;
  if (macho_view(data, size, &mv) != 0)
    return NULL;
  *out_sz = mv.size;
  return mv.base;
}

int symtab_build_from_macho(SymTable *st, const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s) {
    fprintf(stderr, "symtab: not a Mach-O\n");
    return -1;
  }

  MachoView mv;
  if (macho_view(s, sl, &mv) != 0) {
    fprintf(stderr, "symtab: not a Mach-O\n");
    return -1;
  }

  const struct symtab_command *sc = NULL;
  for (MACHO_EACH_LC(&mv, lc)) {
    if (((const struct load_command *)lc)->cmd == LC_SYMTAB)
      sc = (const struct symtab_command *)lc;
  }

  if (!sc) {
    fprintf(stderr, "symtab: no LC_SYMTAB\n");
    return -1;
  }

  const uint8_t *nl = s + sc->symoff;
  const char *strtab = (const char *)(s + sc->stroff);
  for (uint32_t i = 0; i < sc->nsyms; i++) {
    MachoSym n;
    macho_sym_get(&mv, nl, i, &n);
    if (n.n_type & N_STAB)
      continue;   /* debug entry, not a real definition */
    if ((n.n_type & N_EXT) && (n.n_type & 0xe) == N_SECT && n.n_value)
      /* ARM: carry the Thumb bit in bit 0, as a function pointer would */
      symtab_add(st, strtab + n.n_strx,
                 n.n_value | (mv.cputype == CPU_TYPE_ARM && (n.n_desc & N_ARM_THUMB_DEF) ? 1 : 0));
  }

  qsort(st->syms, st->count, sizeof(Sym), sym_cmp);
  fprintf(stderr, "symtab: %zu kernel symbols\n", st->count);

  return 0;
}

uint64_t macho_seg_vmaddr(const uint8_t *data, size_t size, const char *name) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s)
    return 0;

  MachoView mv;
  if (macho_view(s, sl, &mv) != 0)
    return 0;

  for (MACHO_EACH_LC(&mv, lc)) {
    if (!macho_is_segment(&mv, lc))
      continue;
    MachoSeg sg;
    macho_seg_get(&mv, lc, &sg);
    if (!strncmp(sg.segname, name, 16))
      return sg.vmaddr;
  }

  return 0;
}

uint64_t macho_image_vm_extent(const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s)
    return 0;

  MachoView mv;
  if (macho_view(s, sl, &mv) != 0)
    return 0;

  uint64_t lo = UINT64_MAX, hi = 0;
  for (MACHO_EACH_LC(&mv, lc)) {
    if (!macho_is_segment(&mv, lc))
      continue;
    MachoSeg sg;
    macho_seg_get(&mv, lc, &sg);
    if (!sg.vmsize)
      continue;
    if (sg.vmaddr < lo) lo = sg.vmaddr;
    if (sg.vmaddr + sg.vmsize > hi) hi = sg.vmaddr + sg.vmsize;
  }

  return (lo == UINT64_MAX || hi <= lo) ? 0 : hi - lo;
}

int macho_is_arm64(const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s || sl < sizeof(struct mach_header_64))
    return 0;
  const struct mach_header_64 *mh = (const struct mach_header_64 *)s;
  return mh->magic == MH_MAGIC_64 && mh->cputype == CPU_TYPE_ARM64;
}

int macho_uses_arm_kc_layout(const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *slice = find_slice(data, size, &sl);
  if (!slice || sl < sizeof(struct mach_header_64))
    return 0;
  const struct mach_header_64 *mh = (const struct mach_header_64 *)slice;
  return mh->magic == MH_MAGIC_64 &&
         (mh->cputype == CPU_TYPE_ARM64 || mh->cputype == CPU_TYPE_RISCV64);
}

uint64_t macho_kernel_vm_end(const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s)
    return 0;

  MachoView mv;
  if (macho_view(s, sl, &mv) != 0)
    return 0;

  uint64_t highest = 0;
  for (MACHO_EACH_LC(&mv, lc)) {
    if (!macho_is_segment(&mv, lc))
      continue;
    MachoSeg sg;
    macho_seg_get(&mv, lc, &sg);
    if (sg.vmsize > 0 && sg.vmaddr + sg.vmsize > highest)
      highest = sg.vmaddr + sg.vmsize;
  }

  return highest;
}

uint64_t macho_exec_vm_end(const uint8_t *data, size_t size) {
  size_t sl;
  const uint8_t *s = find_slice(data, size, &sl);
  if (!s)
    return 0;

  MachoView mv;
  if (macho_view(s, sl, &mv) != 0)
    return 0;

  uint64_t highest = 0;
  for (MACHO_EACH_LC(&mv, lc)) {
    if (!macho_is_segment(&mv, lc))
      continue;
    MachoSeg sg;
    macho_seg_get(&mv, lc, &sg);
    if ((sg.initprot & VM_PROT_EXECUTE) != 0 && sg.vmsize > 0 &&
        sg.vmaddr + sg.vmsize > highest)
      highest = sg.vmaddr + sg.vmsize;
  }
  return highest;
}

int symtab_merge_from_linked(SymTable *st, const LinkedKext *lk) {
  if (!st ||
    !lk ||
    !lk->linked_data ||
    lk->linked_size < sizeof(struct mach_header))
  {
    return 0;
  }

  const uint8_t *data = lk->linked_data;
  size_t size = lk->linked_size;

  MachoView mv;
  if (macho_view(data, size, &mv) != 0)
    return -1;

  const struct symtab_command *symcmd = NULL;
  const uint8_t *lcp = data + mv.hdrsize;

  for (uint32_t i = 0; i < mv.ncmds; i++) {
    size_t lc_offset = (size_t)(lcp - data);

    if (lc_offset > size ||
      size - lc_offset < sizeof(struct load_command)) {
      return -1;
    }

    uint32_t cmd = r32(lcp);
    uint32_t cmdsize = r32(lcp + 4);

    if (cmdsize < sizeof(struct load_command) ||
      cmdsize > size - lc_offset) {
      return -1;
    }

    if (cmd == LC_SYMTAB) {
      if (cmdsize < sizeof(struct symtab_command))
        return -1;

      symcmd =
        (const struct symtab_command *)lcp;
    }

    lcp += cmdsize;
  }

  if (!symcmd)
    return 0;

  uint64_t symbols_end =
    (uint64_t)symcmd->symoff +
    (uint64_t)symcmd->nsyms *
      macho_nlist_size(&mv);

  uint64_t strings_end =
    (uint64_t)symcmd->stroff +
    symcmd->strsize;

  if (symbols_end > size || strings_end > size)
    return -1;

  const uint8_t *symbols = data + symcmd->symoff;

  const char *string_table =
    (const char *)(data + symcmd->stroff);

  size_t added = 0;

  for (uint32_t i = 0; i < symcmd->nsyms; i++) {
    MachoSym sym;
    macho_sym_get(&mv, symbols, i, &sym);
    const MachoSym *n = &sym;

    if (n->n_type & N_STAB)
      continue;

    if (!(n->n_type & N_EXT))
      continue;

    if ((n->n_type & N_TYPE) != N_SECT)
      continue;

    if (!n->n_value || n->n_sect == NO_SECT)
      continue;

    if (n->n_strx >= symcmd->strsize)
      continue;

    const char *name =
      string_table + n->n_strx;

    size_t remaining =
      symcmd->strsize - n->n_strx;

    if (!memchr(name, '\0', remaining))
      continue;

    /*
     * These belong to each individual kext and must never satisfy an
     * undefined reference from another kext.
     */
    if (!strcmp(name, "_kmod_info") ||
      !strcmp(name, "kmod_info") ||
      !strcmp(name, "__realmain") ||
      !strcmp(name, "__antimain") ||
      !strcmp(name, "__kext_apple_cc")) {
      continue;
    }

    symtab_add(st, name,
               n->n_value | (mv.cputype == CPU_TYPE_ARM && (n->n_desc & N_ARM_THUMB_DEF) ? 1 : 0));
    added++;
  }

  if (added)
    qsort(st->syms,
        st->count,
        sizeof(Sym),
        sym_cmp);

  fprintf(stderr,
      " merged %zu symbols from %s\n",
      added,
      lk->bundle_id
        ? lk->bundle_id
        : "<unknown>");

  return 0;
}

void symtab_free(SymTable *st) {
  for (size_t i = 0; i < st->count; i++)
    free(st->syms[i].name);
  free(st->syms);
  memset(st, 0, sizeof(*st));
}

/* Give the image a file layout matching its VM layout, zero-filling the holes.
 * ld64 packs the file, so __bss (vmsize > filesize) leaves everything after it
 * at a lower file offset than its VA, and the KC then reads __bss as
 * __LINKEDIT's symbol table. */
static int relayout_va_consistent(uint8_t **bufp, size_t *sizep) {
  MachoView mv;
  uint8_t *buf = *bufp;

  if (macho_view(buf, *sizep, &mv) != 0)
    return -1;

  /* Image bounds, and whether anything is out of step. */
  uint64_t image_start = UINT64_MAX;
  uint64_t image_end = 0;
  bool needs_relayout = false;

  for (MACHO_EACH_LC(&mv, lcc)) {
    uint8_t *lcp = (uint8_t *)lcc;
    if (!macho_is_segment(&mv, lcp))
      continue;

    MachoSeg sg;
    macho_seg_get(&mv, lcp, &sg);
    if (sg.vmsize == 0)
      continue;

    if (sg.vmaddr < image_start)
      image_start = sg.vmaddr;
    if (sg.vmaddr + sg.vmsize > image_end)
      image_end = sg.vmaddr + sg.vmsize;
    if (sg.filesize > sg.vmsize)
      return -1;   /* more file than address space: not something to rewrite */
  }

  if (image_start == UINT64_MAX || image_end <= image_start)
    return 0;   /* nothing mapped; leave it alone */

  for (MACHO_EACH_LC(&mv, lcc)) {
    uint8_t *lcp = (uint8_t *)lcc;
    if (!macho_is_segment(&mv, lcp))
      continue;

    MachoSeg sg;
    macho_seg_get(&mv, lcp, &sg);
    if (sg.vmsize == 0)
      continue;
    if (sg.fileoff != sg.vmaddr - image_start) {
      needs_relayout = true;
      break;
    }
  }

  if (!needs_relayout)
    return 0;

  size_t new_size = (size_t)(image_end - image_start);
  uint8_t *nbuf = calloc(1, new_size);
  if (!nbuf)
    return -1;

  /* Copy each segment to its VA-derived offset, recording the shift so file
   * offsets in other load commands can follow. */
  struct { uint64_t old_off, old_end, delta; } shift[64];
  unsigned nshift = 0;

  for (MACHO_EACH_LC(&mv, lcc)) {
    uint8_t *lcp = (uint8_t *)lcc;
    if (!macho_is_segment(&mv, lcp))
      continue;

    MachoSeg sg;
    macho_seg_get(&mv, lcp, &sg);
    if (sg.vmsize == 0)
      continue;

    uint64_t noff = sg.vmaddr - image_start;

    if (sg.filesize != 0) {
      if (sg.fileoff + sg.filesize > *sizep || noff + sg.filesize > new_size) {
        free(nbuf);
        return -1;
      }
      memcpy(nbuf + noff, buf + sg.fileoff, (size_t)sg.filesize);

      if (nshift < 64) {
        shift[nshift].old_off = sg.fileoff;
        shift[nshift].old_end = sg.fileoff + sg.filesize;
        shift[nshift].delta = noff - sg.fileoff;
        nshift++;
      }
    }
  }

  /* Header and load commands came across with __TEXT; edit them in place. */
  MachoView nv;
  if (macho_view(nbuf, new_size, &nv) != 0) {
    free(nbuf);
    return -1;
  }

  for (MACHO_EACH_LC(&nv, lcc)) {
    uint8_t *lcp = (uint8_t *)lcc;
    uint32_t cmd = ((const struct load_command *)lcp)->cmd;

    if (macho_is_segment(&nv, lcp)) {
      MachoSeg sg;
      macho_seg_get(&nv, lcp, &sg);
      if (sg.vmsize == 0)
        continue;

      uint64_t noff = sg.vmaddr - image_start;
      macho_seg_set_addrs(&nv, lcp, sg.vmaddr, sg.vmsize, noff, sg.filesize);

      uint8_t *secs = lcp + macho_seg_cmdsize(&nv);
      for (uint32_t j = 0; j < sg.nsects; j++) {
        uint8_t *sp = secs + (size_t)j * macho_sect_size(&nv);
        MachoSect sec;
        macho_sect_get(&nv, sp, &sec);

        /* Zerofill sections have no file bytes; offset stays 0. */
        uint32_t section_type = sec.flags & SECTION_TYPE;
        bool zerofill = (section_type == S_ZEROFILL)
#ifdef S_GB_ZEROFILL
                        || (section_type == S_GB_ZEROFILL)
#endif
#ifdef S_THREAD_LOCAL_ZEROFILL
                        || (section_type == S_THREAD_LOCAL_ZEROFILL)
#endif
                        ;
        uint32_t noffset = zerofill ? 0 : (uint32_t)(sec.addr - image_start);
        macho_sect_set(&nv, sp, sec.addr, noffset, sec.reloff);
      }
      continue;
    }

#define SHIFT_FIELD(field)                                                    \
    do {                                                                      \
      uint32_t _v = (field);                                                  \
      if (_v != 0) {                                                          \
        for (unsigned _i = 0; _i < nshift; _i++) {                            \
          if (_v >= shift[_i].old_off && _v < shift[_i].old_end) {            \
            (field) = (uint32_t)(_v + shift[_i].delta);                       \
            break;                                                            \
          }                                                                   \
        }                                                                     \
      }                                                                       \
    } while (0)

    if (cmd == LC_SYMTAB) {
      struct symtab_command *sc = (struct symtab_command *)lcp;
      SHIFT_FIELD(sc->symoff);
      SHIFT_FIELD(sc->stroff);
    } else if (cmd == LC_DYSYMTAB) {
      struct dysymtab_command *dc = (struct dysymtab_command *)lcp;
      SHIFT_FIELD(dc->tocoff);
      SHIFT_FIELD(dc->modtaboff);
      SHIFT_FIELD(dc->extrefsymoff);
      SHIFT_FIELD(dc->indirectsymoff);
      SHIFT_FIELD(dc->extreloff);
      SHIFT_FIELD(dc->locreloff);
    } else if (cmd == LC_FUNCTION_STARTS || cmd == LC_DATA_IN_CODE ||
               cmd == LC_DYLD_CHAINED_FIXUPS || cmd == LC_DYLD_EXPORTS_TRIE) {
      struct linkedit_data_command *ld = (struct linkedit_data_command *)lcp;
      SHIFT_FIELD(ld->dataoff);
    } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
      struct dyld_info_command *di = (struct dyld_info_command *)lcp;
      SHIFT_FIELD(di->rebase_off);
      SHIFT_FIELD(di->bind_off);
      SHIFT_FIELD(di->weak_bind_off);
      SHIFT_FIELD(di->lazy_bind_off);
      SHIFT_FIELD(di->export_off);
    }
#undef SHIFT_FIELD
  }

  free(buf);
  *bufp = nbuf;
  *sizep = new_size;
  return 0;
}

static uint64_t va_to_foff(
  const uint8_t *buf,
  size_t size,
  uint64_t va,
  size_t access_size)
{
  if (!buf || size < sizeof(struct mach_header))
    return UINT64_MAX;

  MachoView mv;
  if (macho_view(buf, size, &mv) != 0)
    return UINT64_MAX;

  const uint8_t *lcp = buf + mv.hdrsize;

  for (uint32_t i = 0; i < mv.ncmds; i++) {
    size_t lc_offset = (size_t)(lcp - buf);

    if (lc_offset > size ||
      size - lc_offset < sizeof(struct load_command)) {
      return UINT64_MAX;
    }

    uint32_t cmd = r32(lcp);
    uint32_t cmdsize = r32(lcp + 4);

    if (cmdsize < sizeof(struct load_command) ||
      cmdsize > size - lc_offset) {
      return UINT64_MAX;
    }

    if (cmd == macho_lc_segment(&mv)) {
      if (cmdsize < macho_seg_cmdsize(&mv))
        return UINT64_MAX;

      MachoSeg segv;
      macho_seg_get(&mv, lcp, &segv);
      const MachoSeg *seg = &segv;

      size_t required =
        macho_seg_cmdsize(&mv) +
        (size_t)seg->nsects * macho_sect_size(&mv);

      if (required > cmdsize)
        return UINT64_MAX;

      const uint8_t *sections = lcp + macho_seg_cmdsize(&mv);

      /*
       * Prefer section mappings. Segment filesize does not necessarily
       * describe a simple VA-to-file mapping when a segment contains
       * gaps or zerofill sections.
       */
      for (uint32_t j = 0; j < seg->nsects; j++) {
        MachoSect secv;
        macho_sect_get(&mv, sections + (size_t)j * macho_sect_size(&mv), &secv);
        const MachoSect *sec = &secv;
        uint32_t section_type = sec->flags & SECTION_TYPE;

        if (section_type == S_ZEROFILL
#ifdef S_GB_ZEROFILL
          || section_type == S_GB_ZEROFILL
#endif
#ifdef S_THREAD_LOCAL_ZEROFILL
          || section_type == S_THREAD_LOCAL_ZEROFILL
#endif
        ) {
          continue;
        }

        if (va < sec->addr)
          continue;

        uint64_t delta = va - sec->addr;

        if (delta > sec->size)
          continue;

        if ((uint64_t)access_size > sec->size - delta)
          continue;

        uint64_t file_offset =
          (uint64_t)sec->offset + delta;

        if (file_offset > size ||
          access_size > size - (size_t)file_offset) {
          continue;
        }

        return file_offset;
      }

      /*
       * Fall back to the segment mapping for bytes such as the Mach-O
       * header that may not belong to a section.
       */
      if (seg->filesize != 0 &&
        va >= seg->vmaddr) {
        uint64_t delta = va - seg->vmaddr;

        if (delta <= seg->filesize &&
          (uint64_t)access_size <= seg->filesize - delta) {
          uint64_t file_offset = seg->fileoff + delta;

          if (file_offset <= size &&
            access_size <= size - (size_t)file_offset) {
            return file_offset;
          }
        }
      }
    }

    lcp += cmdsize;
  }

  return UINT64_MAX;
}

#define KMOD_OFF_ADDR_64 156
#define KMOD_OFF_SIZE_64 164
#define KMOD_OFF_ADDR_32 148
#define KMOD_OFF_SIZE_32 152

int patch_kmod_info(LinkedKext *lk) {
  if (!lk ||
    !lk->linked_data ||
    lk->linked_size < sizeof(struct mach_header)) {
    fprintf(stderr,
        " patch_kmod_info: invalid linked kext\n");
    return -1;
  }

  MachoView mv;
  if (macho_view(lk->linked_data, lk->linked_size, &mv) != 0) {
    fprintf(stderr,
        " patch_kmod_info: %s is not a Mach-O\n",
        lk->bundle_id ? lk->bundle_id : "<unknown>");
    return -1;
  }
  const uint32_t kmod_off_addr = mv.is64 ? KMOD_OFF_ADDR_64 : KMOD_OFF_ADDR_32;
  const uint32_t kmod_off_size = mv.is64 ? KMOD_OFF_SIZE_64 : KMOD_OFF_SIZE_32;
  const size_t kmod_field = mv.is64 ? sizeof(uint64_t) : sizeof(uint32_t);

  if (!lk->kmod_info_addr) {
    fprintf(stderr,
        " patch_kmod_info: no defined _kmod_info symbol for %s\n",
        lk->bundle_id ? lk->bundle_id : "<unknown>");
    return -1;
  }

  /*
   * A kmod_info symbol belonging to this image must fall inside one of its
   * mapped segments. This catches accidentally resolved _kmod_info imports
   * before they patch an unrelated kext.
   */
  bool kmod_in_image = false;
  uint64_t image_start = UINT64_MAX;
  uint64_t image_end = 0;

  const uint8_t *lcp = lk->linked_data + mv.hdrsize;

  for (uint32_t i = 0; i < mv.ncmds; i++) {
    size_t lc_offset =
      (size_t)(lcp - lk->linked_data);

    if (lc_offset > lk->linked_size ||
      lk->linked_size - lc_offset <
        sizeof(struct load_command)) {
      return -1;
    }

    uint32_t cmd = r32(lcp);
    uint32_t cmdsize = r32(lcp + 4);

    if (cmdsize < sizeof(struct load_command) ||
      cmdsize > lk->linked_size - lc_offset) {
      return -1;
    }

    if (cmd == macho_lc_segment(&mv)) {
      if (cmdsize < macho_seg_cmdsize(&mv))
        return -1;

      MachoSeg segv;
      macho_seg_get(&mv, lcp, &segv);
      const MachoSeg *seg = &segv;

      if (seg->vmsize != 0) {
        uint64_t seg_end;

        if (UINT64_MAX - seg->vmaddr < seg->vmsize)
          return -1;

        seg_end = seg->vmaddr + seg->vmsize;

        if (seg->vmaddr < image_start)
          image_start = seg->vmaddr;

        if (seg_end > image_end)
          image_end = seg_end;

        if (lk->kmod_info_addr >= seg->vmaddr &&
          lk->kmod_info_addr < seg_end) {
          kmod_in_image = true;
        }
      }
    }

    lcp += cmdsize;
  }

  if (!kmod_in_image) {
    fprintf(stderr,
        " patch_kmod_info: %s has foreign kmod_info VA "
        "0x%llx; image range is 0x%llx-0x%llx\n",
        lk->bundle_id ? lk->bundle_id : "<unknown>",
        (unsigned long long)lk->kmod_info_addr,
        (unsigned long long)image_start,
        (unsigned long long)image_end);
    return -1;
  }

  uint64_t addr_va;
  uint64_t size_va;

  if (UINT64_MAX - lk->kmod_info_addr < kmod_off_size + 8)
    return -1;

  addr_va = lk->kmod_info_addr + kmod_off_addr;
  size_va = lk->kmod_info_addr + kmod_off_size;

  uint64_t addr_foff =
    va_to_foff(lk->linked_data,
           lk->linked_size,
           addr_va,
           kmod_field);

  uint64_t size_foff =
    va_to_foff(lk->linked_data,
           lk->linked_size,
           size_va,
           kmod_field);

  if (addr_foff == UINT64_MAX ||
    size_foff == UINT64_MAX) {
    fprintf(stderr,
        " patch_kmod_info: kmod_info VA 0x%llx not "
        "mappable to file offsets for %s\n",
        (unsigned long long)lk->kmod_info_addr,
        lk->bundle_id ? lk->bundle_id : "<unknown>");
    return -1;
  }

  /*
   * kmod_info.address and kmod_info.size describe the loaded module image,
   * not merely __TEXT. Use the complete mapped range.
   */
  if (image_start == UINT64_MAX || image_end <= image_start)
    return -1;

  uint64_t image_size = image_end - image_start;

  lk->vm_extent = image_size;

  if (mv.is64) {
    w64(lk->linked_data + addr_foff, image_start);
    w64(lk->linked_data + size_foff, image_size);
    // address stays unslid on purpose, OSKext adds the kext slide to it itself
  } else {
    w32(lk->linked_data + addr_foff, (uint32_t)image_start);
    w32(lk->linked_data + size_foff, (uint32_t)image_size);
  }

  fprintf(stderr,
      " patched kmod_info: %s "
      "kmod=0x%llx address=0x%llx size=0x%llx\n",
      lk->bundle_id ? lk->bundle_id : "<unknown>",
      (unsigned long long)lk->kmod_info_addr,
      (unsigned long long)image_start,
      (unsigned long long)image_size);

  return 0;
}

uint64_t symtab_lookup(const SymTable *st, const char *name) {
  size_t lo = 0, hi = st->count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = strcmp(st->syms[mid].name, name);
    if (!c)
      return st->syms[mid].addr;
    if (c < 0)
      lo = mid + 1; else hi = mid;
  }
  return 0;
}

static uint64_t find_defined_kmod_info(
    const MachoView *mv,
    const uint8_t *symbols,
    uint32_t symbol_count,
    const char *string_table,
    size_t string_table_size,
    const uint64_t *symbol_addresses)
{
    if (!symbols ||
        !string_table ||
        !symbol_addresses) {
        return 0;
    }

    for (uint32_t i = 0; i < symbol_count; i++) {
        MachoSym sym;
        macho_sym_get(mv, symbols, i, &sym);
        const MachoSym *n = &sym;

        if (n->n_type & N_STAB)
            continue;

        if (n->n_strx >= string_table_size)
            continue;

        const char *name =
            string_table + n->n_strx;

        size_t remaining =
            string_table_size - n->n_strx;

        if (!memchr(name, '\0', remaining))
            continue;

        if (strcmp(name, "_kmod_info") != 0 &&
            strcmp(name, "kmod_info") != 0) {
            continue;
        }

        /*
         * The critical condition: only accept a definition belonging to this
         * Mach-O. Never accept N_UNDF resolved from a prior kext.
         */
        uint8_t type = n->n_type & N_TYPE;

        if (type != N_SECT && type != N_ABS)
            continue;

        if (type == N_SECT && n->n_sect == NO_SECT)
            continue;

        if (!symbol_addresses[i])
            continue;

        return symbol_addresses[i];
    }

    return 0;
}

// final vmaddrs of pointers link_kext wrote, the collection chains them so they slide
typedef struct { uint64_t *v; size_t n, cap; } SlotList;

static void slot_add(SlotList *sl, uint64_t vm) {
  if (!sl)
    return;
  if (sl->n == sl->cap) {
    size_t cap = sl->cap ? sl->cap * 2 : 256;
    uint64_t *v = realloc(sl->v, cap * sizeof(*v));
    if (!v)
      return;
    sl->v = v;
    sl->cap = cap;
  }
  sl->v[sl->n++] = vm;
}

// the file location of an original (unslid) vmaddr, or NULL outside the file
static uint8_t *seg_loc(uint8_t *buf, size_t sl, const SegInfo *seginfo,
                        uint32_t nseginfo, uint64_t vmaddr, uint64_t width) {
  for (uint32_t i = 0; i < nseginfo; i++) {
    if (vmaddr < seginfo[i].vmaddr ||
        vmaddr + width > seginfo[i].vmaddr + seginfo[i].filesize)
      continue;
    uint64_t off = seginfo[i].fileoff + (vmaddr - seginfo[i].vmaddr);
    return off + width <= sl ? buf + off : NULL;
  }
  return NULL;
}

static uint64_t resolve_import(const SymTable *kernel_syms, const char *name,
                               uint64_t trampoline) {
  uint64_t v = symtab_lookup(kernel_syms, name);
  if (!v && name[0] == '_')
    v = symtab_lookup(kernel_syms, name + 1);
  if (!v) {
    fprintf(stderr, "  BIND UNRESOLVED: %s\n", name);
  }
  return v;
}

#define DYLD_CHAINED_PTR_64        2
#define DYLD_CHAINED_PTR_64_OFFSET 6
#define DYLD_CHAINED_PTR_START_NONE_ 0xffff
#define DYLD_CHAINED_IMPORT          1
#define DYLD_CHAINED_IMPORT_ADDEND   2
#define DYLD_CHAINED_IMPORT_ADDEND64 3

// chained fixups as ld64 writes them for riscv64 bundles, each slot becomes a
// plain pointer and the chains are emptied so nothing walks them twice
static int apply_chained_fixups(uint8_t *buf, size_t sl, uint8_t *cmd,
                                const SegInfo *seginfo, uint32_t nseginfo,
                                uint64_t load_vmaddr, const SymTable *kernel_syms,
                                uint64_t trampoline, SlotList *slots) {
  const struct linkedit_data_command *lc = (const struct linkedit_data_command *)cmd;
  if ((uint64_t)lc->dataoff + lc->datasize > sl ||
      lc->datasize < sizeof(struct dyld_chained_fixups_header))
    return -1;
  uint8_t *blob = buf + lc->dataoff;
  const uint8_t *blob_end = blob + lc->datasize;
  struct dyld_chained_fixups_header *h = (struct dyld_chained_fixups_header *)blob;
  if (h->fixups_version != 0 || h->starts_offset + 4 > lc->datasize ||
      h->symbols_offset > lc->datasize)
    return -1;

  uint32_t isize = h->imports_format == DYLD_CHAINED_IMPORT_ADDEND64 ? 16 :
                   h->imports_format == DYLD_CHAINED_IMPORT_ADDEND ? 8 : 4;
  if (h->imports_format < DYLD_CHAINED_IMPORT ||
      h->imports_format > DYLD_CHAINED_IMPORT_ADDEND64 ||
      (uint64_t)h->imports_offset + (uint64_t)h->imports_count * isize > lc->datasize) {
    fprintf(stderr, "  chained fixups: bad imports table\n");
    return -1;
  }
  // every import resolved up front, name and addend
  uint64_t *import_vals = calloc(h->imports_count ? h->imports_count : 1, sizeof(uint64_t));
  if (!import_vals)
    return -1;
  const char *syms = (const char *)blob + h->symbols_offset;
  for (uint32_t i = 0; i < h->imports_count; i++) {
    const uint8_t *e = blob + h->imports_offset + (size_t)i * isize;
    uint64_t name_off, addend = 0;
    if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND64) {
      name_off = r64(e) >> 32;
      addend = r64(e + 8);
    } else {
      name_off = r32(e) >> 9;
      if (h->imports_format == DYLD_CHAINED_IMPORT_ADDEND)
        addend = (uint64_t)(int64_t)(int32_t)r32(e + 4);
    }
    if ((const uint8_t *)syms + name_off >= blob_end) {
      free(import_vals);
      return -1;
    }
    uint64_t resolved = resolve_import(kernel_syms, syms + name_off, trampoline);
    if (!resolved) {
      free(import_vals);
      return -1;
    }
    import_vals[i] = resolved + addend;
  }

  uint8_t *starts = blob + h->starts_offset;
  uint32_t seg_count = r32(starts);
  uint64_t image_base = nseginfo ? seginfo[0].vmaddr : 0;
  int rc = 0;
  for (uint32_t si = 0; si < seg_count && si < nseginfo && !rc; si++) {
    uint32_t seg_off = r32(starts + 4 + 4 * si);
    if (!seg_off)
      continue;
    const uint8_t *s = starts + seg_off;
    if (s + 22 > blob_end) { rc = -1; break; }
    uint16_t page_size = *(const uint16_t *)(s + 4);
    uint16_t format = *(const uint16_t *)(s + 6);
    uint64_t segment_offset = r64(s + 8);
    uint16_t page_count = *(const uint16_t *)(s + 20);
    if (format != DYLD_CHAINED_PTR_64 && format != DYLD_CHAINED_PTR_64_OFFSET) {
      fprintf(stderr, "  chained fixups: pointer format %u not supported\n", format);
      rc = -1;
      break;
    }
    for (uint32_t pi = 0; pi < page_count; pi++) {
      uint16_t start = *(const uint16_t *)(s + 22 + 2 * pi);
      if (start == DYLD_CHAINED_PTR_START_NONE_)
        continue;
      uint64_t vm = image_base + segment_offset + (uint64_t)pi * page_size + start;
      for (;;) {
        uint8_t *loc = seg_loc(buf, sl, seginfo, nseginfo, vm, 8);
        if (!loc) { rc = -1; break; }
        uint64_t raw = r64(loc);
        uint64_t next = (raw >> 51) & 0xfff;
        uint64_t val;
        if (raw >> 63) {
          uint32_t ordinal = raw & 0xffffff;
          uint64_t addend = (raw >> 24) & 0xff;
          if (ordinal >= h->imports_count) { rc = -1; break; }
          val = import_vals[ordinal] + addend;
        } else {
          uint64_t target = raw & 0xfffffffffULL;
          uint64_t high8 = (raw >> 36) & 0xff;
          if (format == DYLD_CHAINED_PTR_64_OFFSET)
            target += image_base;
          val = (high8 << 56) | (target + load_vmaddr);
        }
        w64(loc, val);
        slot_add(slots, vm + load_vmaddr);
        if (!next)
          break;
        vm += next * 4;
      }
      if (rc)
        break;
    }
  }
  free(import_vals);
  if (rc) {
    fprintf(stderr, "  chained fixups: malformed chain\n");
    return rc;
  }
  // the slots hold final values now, an empty starts table keeps them that way
  w32(starts, 0);
  return 0;
}

// mh_kext_bundle keeps its fixups as classic external and local relocations,
// pointers only since calls out of a riscv or arm64 kext go through stubs
static void apply_dysymtab_relocs(uint8_t *buf, size_t sl, const MachoView *mv,
                                  const struct dysymtab_command *ds,
                                  const SegInfo *seginfo, uint32_t nseginfo,
                                  const uint64_t *sym_addrs, uint32_t nsyms,
                                  uint64_t load_vmaddr, uint32_t *nerrs,
                                  SlotList *slots) {
  // external addresses are absolute, local ones count from the first segment
  uint64_t ext_base = 0;
  uint64_t loc_base = nseginfo ? seginfo[0].vmaddr : 0;
  for (int local = 0; local < 2; local++) {
    uint32_t off = local ? ds->locreloff : ds->extreloff;
    uint32_t n = local ? ds->nlocrel : ds->nextrel;
    if (!n || (uint64_t)off + (uint64_t)n * sizeof(struct relocation_info) > sl)
      continue;
    const struct relocation_info *rel = (const struct relocation_info *)(buf + off);
    for (uint32_t i = 0; i < n; i++) {
      struct relocation_info r = rel[i];
      if (mv->cputype == CPU_TYPE_X86_64 && r.r_pcrel &&
          r.r_length == RL_LONG && r.r_extern &&
          (r.r_type == X86_64_RELOC_BRANCH || r.r_type == X86_64_RELOC_SIGNED ||
           r.r_type == X86_64_RELOC_SIGNED_1 || r.r_type == X86_64_RELOC_SIGNED_2 ||
           r.r_type == X86_64_RELOC_SIGNED_4)) {
        uint64_t vm = ext_base + (int32_t)r.r_address;
        uint8_t *loc = seg_loc(buf, sl, seginfo, nseginfo, vm, 4);
        uint64_t target = r.r_symbolnum < nsyms ? sym_addrs[r.r_symbolnum] : 0;
        if (!loc || !target) { (*nerrs)++; continue; }
        unsigned extra = r.r_type == X86_64_RELOC_SIGNED_1 ? 1 :
                         r.r_type == X86_64_RELOC_SIGNED_2 ? 2 :
                         r.r_type == X86_64_RELOC_SIGNED_4 ? 4 : 0;
        int64_t displacement = (int64_t)(target - (load_vmaddr + vm + 4 + extra)) +
                               (int32_t)r32(loc);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
          fprintf(stderr, "  x86_64 kext branch relocation out of range\n");
          (*nerrs)++;
          continue;
        }
        w32(loc, (uint32_t)(int32_t)displacement);
        continue;
      }
      if (r.r_type != 0 || r.r_pcrel || r.r_length < RL_LONG ||
          (int32_t)r.r_address < 0) {
        fprintf(stderr, "  WARN: unhandled %s relocation type %u\n",
                local ? "local" : "external", r.r_type);
        (*nerrs)++;
        continue;
      }
      bool is64 = r.r_length == RL_QUAD;
      uint64_t vm = (local ? loc_base : ext_base) + (uint32_t)r.r_address;
      uint8_t *loc = seg_loc(buf, sl, seginfo, nseginfo, vm, is64 ? 8 : 4);
      if (!loc) {
        (*nerrs)++;
        continue;
      }
      if (r.r_extern) {
        uint64_t target = r.r_symbolnum < nsyms ? sym_addrs[r.r_symbolnum] : 0;
        if (!target)
          (*nerrs)++;
        wptr(loc, rptr(loc, is64) + target, is64);
      } else {
        wptr(loc, rptr(loc, is64) + load_vmaddr, is64);
      }
      if (is64)
        slot_add(slots, vm + load_vmaddr);
    }
  }
}

int link_kext(const uint8_t *kext_data, size_t kext_size,
              const SymTable *kernel_syms,
              uint64_t load_vmaddr,
              LinkedKext *out)
{
  size_t sl;
  const uint8_t *slice = find_slice(kext_data, kext_size, &sl);
  if (!slice) {
    fprintf(stderr, "link_kext: not a Mach-O\n");
    return -1;
  }

  uint8_t *buf = malloc(sl);
  if (!buf)
    return -1;
  memcpy(buf, slice, sl);

  MachoView mv;
  if (macho_view(buf, sl, &mv) != 0) {
    fprintf(stderr, "link_kext: not a Mach-O\n");
    free(buf);
    return -1;
  }
  const int is_arm32 = (mv.cputype == CPU_TYPE_ARM);
  SlotList slots = {0};

  const struct symtab_command *symcmd = NULL;
  const uint8_t *dyld_info = NULL;   // LC_DYLD_INFO(_ONLY) command
  uint8_t *chained = NULL;           // LC_DYLD_CHAINED_FIXUPS command
  const struct dysymtab_command *dysym = NULL;
  SegInfo seginfo[32];
  uint32_t nseginfo = 0;

  for (MACHO_EACH_LC(&mv, lcc)) {
    uint8_t *lcp = (uint8_t *)lcc;
    uint32_t cmd = ((const struct load_command *)lcp)->cmd;
    if (macho_is_segment(&mv, lcp)) {
      MachoSeg sg;
      macho_seg_get(&mv, lcp, &sg);
      macho_seg_set_addrs(&mv, lcp, sg.vmaddr + load_vmaddr, sg.vmsize,
                          sg.fileoff, sg.filesize);
      uint8_t *secs = lcp + macho_seg_cmdsize(&mv);
      for (uint32_t j = 0; j < sg.nsects; j++) {
        uint8_t *sp = secs + (size_t)j * macho_sect_size(&mv);
        MachoSect sec;
        macho_sect_get(&mv, sp, &sec);
        macho_sect_set(&mv, sp, sec.addr + load_vmaddr, sec.offset, sec.reloff);
      }
      if (nseginfo < 32) {
        seginfo[nseginfo].fileoff = sg.fileoff;
        seginfo[nseginfo].vmaddr = sg.vmaddr;
        seginfo[nseginfo].filesize = sg.filesize;
        nseginfo++;
      }
    } else if (cmd == LC_SYMTAB) {
      symcmd = (const struct symtab_command *)lcp;
    } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
      dyld_info = lcp;
    } else if (cmd == LC_DYLD_CHAINED_FIXUPS) {
      chained = lcp;
    } else if (cmd == LC_DYSYMTAB) {
      dysym = (const struct dysymtab_command *)lcp;
    }
  }

  if (!symcmd) {
    fprintf(stderr, "link_kext: no LC_SYMTAB\n");
    free(buf);
    return -1;
  }

  uint8_t *nl = buf + symcmd->symoff;
  const char *strtab = (const char *)(buf + symcmd->stroff);
  uint32_t nsyms = symcmd->nsyms;

  uint64_t *sym_addrs = calloc(nsyms, sizeof(uint64_t));

  for (uint32_t i = 0; i < nsyms; i++) {
    MachoSym sym;
    macho_sym_get(&mv, nl, i, &sym);
    const MachoSym *n = &sym;
    uint8_t type = n->n_type & 0x0e;
    if (type == N_SECT) {
      // n_value is the symbol's vmaddr in the *original* (pre-slide) address space.
      // After the segment patch loop above, section.addr = original + load_vmaddr.
      //
      // So the symbol's final VA = (n_value - original_seg_vmaddr) + final_seg_vmaddr.
      // For kexts compiled at vmbase 0 this reduces to load_vmaddr + n_value, which is fine.
      // But store it as-is and let the write-back be the canonical source
      macho_sym_set_value(&mv, nl, i, load_vmaddr + n->n_value);
      sym_addrs[i] = load_vmaddr + n->n_value;
      if (is_arm32 && (n->n_desc & N_ARM_THUMB_DEF))
        sym_addrs[i] |= 1;
    } else if (type == N_ABS) {
      sym_addrs[i] = n->n_value;
    } else if (type == N_UNDF && !(n->n_type & N_STAB)) {
      /*
       * A debug (stab) entry can look like N_UNDF - a compiler emits an
       * N_GSYM alongside each global, carrying the same name with no value.
       * Resolving those as imports reports every -g build's globals as
       * unresolved.
       */
      const char *name = strtab + n->n_strx;
      uint64_t ka = symtab_lookup(kernel_syms, name);
      if (!ka && name[0] == '_')
        ka = symtab_lookup(kernel_syms, name + 1);
      if (ka)
        sym_addrs[i] = ka;
      else
        fprintf(stderr, "  UNRESOLVED: %s\n", name);
    }
  }

  typedef struct { uint32_t foff; uint64_t vmaddr; uint32_t size; } SecRef;
  SecRef *sec_refs = NULL;
  uint32_t total_sects = 0;
  {
    for (MACHO_EACH_LC(&mv, p)) {
      if (macho_is_segment(&mv, p)) {
        MachoSeg sg;
        macho_seg_get(&mv, p, &sg);
        total_sects += sg.nsects;
      }
    }
    sec_refs = calloc(total_sects, sizeof(SecRef));
    uint32_t si = 0;
    for (MACHO_EACH_LC(&mv, p)) {
      if (!macho_is_segment(&mv, p))
        continue;
      MachoSeg sg;
      macho_seg_get(&mv, p, &sg);
      const uint8_t *secs = p + macho_seg_cmdsize(&mv);
      for (uint32_t j = 0; j < sg.nsects; j++) {
        MachoSect sec;
        macho_sect_get(&mv, secs + (size_t)j * macho_sect_size(&mv), &sec);
        sec_refs[si].foff   = sec.offset;
        sec_refs[si].vmaddr = sec.addr;
        sec_refs[si].size   = (uint32_t)sec.size;
        si++;
      }
    }
  }

  uint32_t nerrs = 0;
  {
    for (MACHO_EACH_LC(&mv, p)) {
      if (!macho_is_segment(&mv, p))
        continue;
      MachoSeg sg;
      macho_seg_get(&mv, p, &sg);
      const uint8_t *secs = p + macho_seg_cmdsize(&mv);
      for (uint32_t j = 0; j < sg.nsects; j++) {
        MachoSect sec;
        macho_sect_get(&mv, secs + (size_t)j * macho_sect_size(&mv), &sec);
        if (!sec.nreloc || !sec.reloff)
          continue;

        const struct relocation_info *relocs =
            (const struct relocation_info *)(buf + sec.reloff);
        for (uint32_t ri = 0; ri < sec.nreloc; ri++) {
          struct relocation_info r = relocs[ri];

          /*
           * ARM keeps the difference of two addresses in a SECTDIFF/PAIR
           * couple rather than x86_64's SUBTRACTOR, and scattered entries
           * (r_address bit 31) encode the value inline.
           */
          if (is_arm32) {
            if (r.r_type == ARM_RELOC_PAIR)
              continue;   /* consumed with its principal relocation */

            if (r.r_type == ARM_RELOC_SECTDIFF ||
                r.r_type == ARM_RELOC_LOCAL_SECTDIFF) {
              if (ri + 1 >= sec.nreloc)
                continue;
              struct relocation_info r2 = relocs[++ri];
              if ((uint32_t)r.r_address + 4 > sec.size)
                continue;
              uint8_t *loc = buf + sec.offset + r.r_address;
              /*
               * Both halves name sections by index; the stored value already
               * holds (from - to) computed at the original addresses, so the
               * slide cancels and only a section move needs correcting.
               */
              uint64_t from = (r.r_symbolnum && r.r_symbolnum <= total_sects)
                                  ? sec_refs[r.r_symbolnum - 1].vmaddr : 0;
              uint64_t to = (r2.r_symbolnum && r2.r_symbolnum <= total_sects)
                                ? sec_refs[r2.r_symbolnum - 1].vmaddr : 0;
              (void)from;
              (void)to;
              continue;
            }

            if ((uint32_t)r.r_address >= sec.size)
              continue;

            uint8_t *loc = buf + sec.offset + r.r_address;
            uint64_t pc = sec.addr + (uint32_t)r.r_address;
            uint64_t target = 0;
            if (r.r_extern) {
              if (r.r_symbolnum < nsyms)
                target = sym_addrs[r.r_symbolnum];
              if (!target)
                nerrs++;
            } else if (r.r_symbolnum >= 1 && r.r_symbolnum <= total_sects) {
              target = sec_refs[r.r_symbolnum - 1].vmaddr;
            }

            switch (r.r_type) {
            case ARM_RELOC_VANILLA: {
              if (r.r_length == RL_LONG)
                w32(loc, (uint32_t)(r32(loc) + target));
              else
                fprintf(stderr, "  WARN: ARM_RELOC_VANILLA length %u\n",
                        r.r_length);
              break;
            }
            case ARM_RELOC_BR24: {
              /* B/BL: signed 24-bit word displacement, pc reads as insn + 8. */
              uint32_t insn = r32(loc);
              int32_t addend = (int32_t)((insn & 0x00ffffffu) << 8) >> 6;
              int thumb = (int)(target & 1);
              int64_t rel = (int64_t)(target & ~1ULL) + addend - (int64_t)(pc + 8);
              if (thumb) {
                /* ARM caller, Thumb callee: BL becomes BLX (imm), H is offset bit 1 */
                if ((insn & 0x0f000000u) != 0x0b000000u || (insn >> 28) != 0xe ||
                    (rel & 1) || rel < -33554432LL || rel > 33554430LL) {
                  fprintf(stderr, "  WARN: BR24 to Thumb not convertible (insn=%08x rel=%lld)\n",
                          insn, (long long)rel);
                  nerrs++;
                  break;
                }
                insn = 0xfa000000u | ((uint32_t)((rel >> 1) & 1) << 24) |
                       (((uint32_t)(rel >> 2)) & 0x00ffffffu);
                w32(loc, insn);
                break;
              }
              if ((rel & 3) || rel < -33554432LL || rel > 33554428LL) {
                fprintf(stderr, "  WARN: BR24 out of range (rel=%lld)\n",
                        (long long)rel);
                nerrs++;
                break;
              }
              insn = (insn & 0xff000000u) | (((uint32_t)(rel >> 2)) & 0x00ffffffu);
              w32(loc, insn);
              break;
            }
            default:
              fprintf(stderr, "  WARN: unhandled arm reloc type %u\n",
                      r.r_type);
              break;
            }
            continue;
          }

          // final riscv images never carry section relocations
          if (mv.cputype == CPU_TYPE_RISCV64) {
            fprintf(stderr, "  WARN: riscv section relocation type %u left unapplied\n",
                    r.r_type);
            continue;
          }

          if (r.r_type == X86_64_RELOC_SUBTRACTOR) {
            if (ri + 1 >= sec.nreloc) continue;
            struct relocation_info r2 = relocs[++ri];
            if ((uint32_t)r.r_address + 8 > sec.size)
              continue;
            uint8_t *loc = buf + sec.offset + r.r_address;
            uint64_t sym_b = r.r_extern  ? sym_addrs[r.r_symbolnum]
                                         : (r.r_symbolnum  ? sec_refs[r.r_symbolnum - 1].vmaddr  : load_vmaddr);
            uint64_t sym_a = r2.r_extern ? sym_addrs[r2.r_symbolnum]
                                         : (r2.r_symbolnum ? sec_refs[r2.r_symbolnum - 1].vmaddr : load_vmaddr);
            uint64_t addend = (r2.r_length == RL_QUAD) ? r64(loc) : (uint64_t)(uint32_t)r32(loc);
            uint64_t val = sym_a - sym_b + addend;
            if (r2.r_length == RL_QUAD)
              w64(loc, val);
            else
              w32(loc, (uint32_t)val);
            continue;
          }

          if ((uint32_t)r.r_address >= sec.size)
            continue;

          uint8_t *loc = buf + sec.offset + r.r_address;
          uint64_t pc  = sec.addr + (uint32_t)r.r_address;

          uint64_t target = 0;
          if (r.r_extern) {
            if (r.r_symbolnum < nsyms)
              target = sym_addrs[r.r_symbolnum];
            if (!target) nerrs++;
          } else {
            uint32_t si2 = r.r_symbolnum;
            if (si2 >= 1 && si2 <= total_sects)
              target = sec_refs[si2 - 1].vmaddr;
          }

          switch (r.r_type) {
          case X86_64_RELOC_UNSIGNED: {
            uint64_t addend = r64(loc);
            w64(loc, target + addend);
            break;
          }
          case X86_64_RELOC_BRANCH:
          case X86_64_RELOC_SIGNED:
          case X86_64_RELOC_SIGNED_1:
          case X86_64_RELOC_SIGNED_2:
          case X86_64_RELOC_SIGNED_4: {
            int extra = (r.r_type == X86_64_RELOC_SIGNED_1) ? 1 :
                        (r.r_type == X86_64_RELOC_SIGNED_2) ? 2 :
                        (r.r_type == X86_64_RELOC_SIGNED_4) ? 4 : 0;
            int32_t addend = (int32_t)r32(loc);
            int64_t rel = (int64_t)target - (int64_t)(pc + 4 + extra) + addend;
            w32(loc, (uint32_t)(int32_t)rel);
            break;
          }
          case X86_64_RELOC_GOT_LOAD:
          case X86_64_RELOC_GOT: {
            int32_t addend = (int32_t)r32(loc);
            int64_t rel = (int64_t)target - (int64_t)(pc + 4) + addend;
            if (rel < -2147483648LL || rel > 2147483647LL)
              fprintf(stderr, "  WARN: GOT ref out of 32-bit range (rel=%lld)\n", (long long)rel);
            w32(loc, (uint32_t)(int32_t)rel);
            break;
          }
          default:
            fprintf(stderr, "  WARN: unhandled reloc type %u\n", r.r_type);
            break;
          }
        }
      }
    }
  }

  if (dysym && macho_hdr_filetype(&mv, buf) == MH_KEXT_BUNDLE)
    apply_dysymtab_relocs(buf, sl, &mv, dysym, seginfo, nseginfo, sym_addrs,
                          nsyms, load_vmaddr, &nerrs, &slots);

  if (nerrs) {
    fprintf(stderr, "  link_kext: %u unresolved symbols\n", nerrs);
    free(slots.v);
    free(sym_addrs);
    free(sec_refs);
    free(buf);
    return -1;
  }

  if (chained && nseginfo) {
    uint64_t trampoline = symtab_lookup(kernel_syms, "_panic");
    if (!trampoline)
      trampoline = symtab_lookup(kernel_syms, "panic");
    if (apply_chained_fixups(buf, sl, chained, seginfo, nseginfo, load_vmaddr,
                             kernel_syms, trampoline, &slots) != 0) {
      free(slots.v);
      free(sym_addrs);
      free(sec_refs);
      free(buf);
      return -1;
    }
  }

  if (dyld_info && nseginfo) {
    const struct dyld_info_command *di = (const struct dyld_info_command *)dyld_info;

    uint64_t trampoline = symtab_lookup(kernel_syms, "_panic");
    if (!trampoline)
      trampoline = symtab_lookup(kernel_syms, "panic");

    // Map a (segment index, segment offset) target to a file location in buf.
    #define TARGET_LOC(seg_ix, seg_off) \
      ((seg_ix) < nseginfo ? buf + seginfo[seg_ix].fileoff + (seg_off) : NULL)
    #define NOTE_SLOT(seg_ix, seg_off) \
      do { if (mv.is64 && (seg_ix) < nseginfo) \
        slot_add(&slots, seginfo[seg_ix].vmaddr + (seg_off) + load_vmaddr); } while (0)

    // Rebase: add the slide (load_vmaddr) to each pointer slot.
    if (di->rebase_size) {
      const uint8_t *p   = buf + di->rebase_off;
      const uint8_t *end = p + di->rebase_size;
      uint32_t seg_ix = 0;
      uint64_t seg_off = 0;
      const uint64_t ptr_size = mv.is64 ? 8 : 4;
      int done = 0;
      while (p < end && !done) {
        uint8_t op  = *p & REBASE_OPCODE_MASK;
        uint8_t imm = *p & REBASE_IMMEDIATE_MASK;
        p++;
        switch (op) {
        case REBASE_OPCODE_DONE:
          done = 1;
          break;
        case REBASE_OPCODE_SET_TYPE_IMM:
          break; // only pointer rebases matter for x86_64 kexts
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          seg_ix = imm;
          seg_off = read_uleb(&p, end);
          break;
        case REBASE_OPCODE_ADD_ADDR_ULEB:
          seg_off += read_uleb(&p, end);
          break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
          seg_off += (uint64_t)imm * ptr_size;
          break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
          for (uint32_t k = 0; k < imm; k++) {
            uint8_t *loc = TARGET_LOC(seg_ix, seg_off);
            if (loc) { wptr(loc, rptr(loc, mv.is64) + load_vmaddr, mv.is64); NOTE_SLOT(seg_ix, seg_off); }
            seg_off += ptr_size;
          }
          break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
          uint64_t cnt = read_uleb(&p, end);
          for (uint64_t k = 0; k < cnt; k++) {
            uint8_t *loc = TARGET_LOC(seg_ix, seg_off);
            if (loc) { wptr(loc, rptr(loc, mv.is64) + load_vmaddr, mv.is64); NOTE_SLOT(seg_ix, seg_off); }
            seg_off += ptr_size;
          }
          break;
        }
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
          uint8_t *loc = TARGET_LOC(seg_ix, seg_off);
          if (loc) { wptr(loc, rptr(loc, mv.is64) + load_vmaddr, mv.is64); NOTE_SLOT(seg_ix, seg_off); }
          seg_off += ptr_size + read_uleb(&p, end);
          break;
        }
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
          uint64_t cnt  = read_uleb(&p, end);
          uint64_t skip = read_uleb(&p, end);
          for (uint64_t k = 0; k < cnt; k++) {
            uint8_t *loc = TARGET_LOC(seg_ix, seg_off);
            if (loc) { wptr(loc, rptr(loc, mv.is64) + load_vmaddr, mv.is64); NOTE_SLOT(seg_ix, seg_off); }
            seg_off += ptr_size + skip;
          }
          break;
        }
        default:
          fprintf(stderr, "  rebase: unknown opcode 0x%x\n", op);
          done = 1;
          break;
        }
      }
    }

    // Bind + lazy bind: resolve each named symbol and write its address.
    for (int pass = 0; pass < 2; pass++) {
      uint32_t boff = pass ? di->lazy_bind_off  : di->bind_off;
      uint32_t bsz  = pass ? di->lazy_bind_size : di->bind_size;
      if (!bsz)
        continue;
      const uint8_t *p   = buf + boff;
      const uint8_t *end = p + bsz;
      uint32_t seg_ix = 0;
      uint64_t seg_off = 0;
      const uint64_t ptr_size = mv.is64 ? 8 : 4;
      int64_t  addend = 0;
      const char *sym = NULL;
      uint64_t symval = 0;
      int done = 0;
      #define RESOLVE_SYM() do { \
        symval = sym ? symtab_lookup(kernel_syms, sym) : 0; \
        if (!symval && sym && sym[0] == '_') symval = symtab_lookup(kernel_syms, sym + 1); \
        if (!symval) { \
          fprintf(stderr, "  BIND UNRESOLVED: %s\n", sym ? sym : "<unnamed>"); \
          nerrs++; \
        } \
      } while (0)
      #define DO_ONE_BIND() do { \
        uint8_t *loc = TARGET_LOC(seg_ix, seg_off); \
        if (loc && symval) { wptr(loc, symval + addend, mv.is64); NOTE_SLOT(seg_ix, seg_off); } \
        seg_off += ptr_size; \
      } while (0)
      while (p < end && !done) {
        uint8_t op  = *p & BIND_OPCODE_MASK;
        uint8_t imm = *p & BIND_IMMEDIATE_MASK;
        p++;
        switch (op) {
        case BIND_OPCODE_DONE:
          if (pass == 1)
            break; // lazy: DONE marks end of each entry, keep going
          done = 1;
          break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
          break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
          read_uleb(&p, end);
          break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
          sym = (const char *)p;
          while (p < end && *p)
            p++;
          if (p < end)
            p++; // skip NUL
          RESOLVE_SYM();
          break;
        case BIND_OPCODE_SET_TYPE_IMM:
          break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
          addend = read_sleb(&p, end);
          break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
          seg_ix = imm; seg_off = read_uleb(&p, end);
          break;
        case BIND_OPCODE_ADD_ADDR_ULEB:
          seg_off += read_uleb(&p, end);
          break;
        case BIND_OPCODE_DO_BIND:
          DO_ONE_BIND();
          break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
          DO_ONE_BIND();
          seg_off += read_uleb(&p, end);
          break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
          DO_ONE_BIND();
          seg_off += (uint64_t)imm * ptr_size;
          break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
          uint64_t cnt  = read_uleb(&p, end);
          uint64_t skip = read_uleb(&p, end);
          for (uint64_t k = 0; k < cnt; k++) {
            uint8_t *loc = TARGET_LOC(seg_ix, seg_off);
            if (loc && symval) { wptr(loc, symval + addend, mv.is64); NOTE_SLOT(seg_ix, seg_off); }
            seg_off += ptr_size + skip;
          }
          break;
        }
        default:
          fprintf(stderr, "  bind: unknown opcode 0x%x\n", op);
          done = 1;
          break;
        }
      }
      #undef RESOLVE_SYM
      #undef DO_ONE_BIND
    }
    #undef TARGET_LOC
    #undef NOTE_SLOT
  }

  size_t strtab_size = 0;

  if ((uint64_t)symcmd->stroff <= sl)
    strtab_size = sl - symcmd->stroff;

  uint64_t kmod_info_addr = find_defined_kmod_info(&mv, nl, nsyms, strtab, strtab_size, sym_addrs);

  free(sym_addrs);
  free(sec_refs);

  if (nerrs) {
    free(slots.v);
    free(buf);
    return -1;
  }

  if (relayout_va_consistent(&buf, &sl) != 0) {
    free(slots.v);
    free(buf);
    return -1;
  }

  out->linked_data = buf;
  out->linked_size = sl;
  out->load_vmaddr = load_vmaddr;
  out->kmod_info_addr = kmod_info_addr;
  out->ptr_slots = slots.v;
  out->nptr_slots = slots.n;
  return 0;
}
