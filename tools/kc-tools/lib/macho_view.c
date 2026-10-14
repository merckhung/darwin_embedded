#include <stdio.h>
#include <string.h>

#include "macho_view.h"


static void copy_name(char *dst, const char *src, size_t n) {
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int macho_view(const uint8_t *data, size_t size, MachoView *mv) {
  if (!data || !mv || size < sizeof(struct mach_header_64)) return -1;
  const struct mach_header_64 *mh = (const struct mach_header_64 *)data;
  if (mh->magic != MH_MAGIC_64 ||
      (mh->cputype != CPU_TYPE_X86_64 && mh->cputype != CPU_TYPE_ARM64) ||
      mh->sizeofcmds > size - sizeof(*mh) || mh->ncmds > mh->sizeofcmds / 8)
    return -1;
  size_t at = sizeof(*mh), end = at + mh->sizeofcmds;
  for (uint32_t i = 0; i < mh->ncmds; ++i) {
    if (end - at < sizeof(struct load_command)) return -1;
    const struct load_command *lc = (const struct load_command *)(data + at);
    if (lc->cmdsize < 8 || (lc->cmdsize & 7) || lc->cmdsize > end - at) return -1;
    if (lc->cmd == LC_SEGMENT_64) {
      if (lc->cmdsize < sizeof(struct segment_command_64)) return -1;
      const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
      if (seg->nsects > (lc->cmdsize - sizeof(*seg)) / sizeof(struct section_64)) return -1;
    }
    at += lc->cmdsize;
  }
  if (at != end) return -1;
  *mv = (MachoView){.base = data, .size = size, .is64 = 1,
      .cputype = mh->cputype, .cpusubtype = mh->cpusubtype,
      .filetype = mh->filetype, .ncmds = mh->ncmds,
      .sizeofcmds = mh->sizeofcmds, .flags = mh->flags,
      .hdrsize = sizeof(*mh), .lc0 = data + sizeof(*mh)};
  return 0;
}

int macho_is_segment(const MachoView *mv, const uint8_t *lc) {
  return ((const struct load_command *)lc)->cmd == macho_lc_segment(mv);
}

void macho_seg_get(const MachoView *mv, const uint8_t *lc, MachoSeg *out) {
  const struct segment_command_64 *sc =
      (const struct segment_command_64 *)lc;
  copy_name(out->segname, sc->segname, 16);
  out->vmaddr = sc->vmaddr;
  out->vmsize = sc->vmsize;
  out->fileoff = sc->fileoff;
  out->filesize = sc->filesize;
  out->maxprot = sc->maxprot;
  out->initprot = sc->initprot;
  out->nsects = sc->nsects;
  out->flags = sc->flags;
  out->sects = sc->nsects ? (const uint8_t *)(sc + 1) : NULL;
}

void macho_seg_set_addrs(const MachoView *mv, uint8_t *lc, uint64_t vmaddr,
                         uint64_t vmsize, uint64_t fileoff, uint64_t filesize) {
  struct segment_command_64 *sc = (struct segment_command_64 *)lc;
  sc->vmaddr = vmaddr;
  sc->vmsize = vmsize;
  sc->fileoff = fileoff;
  sc->filesize = filesize;
}

void macho_sect_get(const MachoView *mv, const uint8_t *sect, MachoSect *out) {
  const struct section_64 *s = (const struct section_64 *)sect;
  copy_name(out->sectname, s->sectname, 16);
  copy_name(out->segname, s->segname, 16);
  out->addr = s->addr;
  out->size = s->size;
  out->offset = s->offset;
  out->align = s->align;
  out->reloff = s->reloff;
  out->nreloc = s->nreloc;
  out->flags = s->flags;
  out->reserved1 = s->reserved1;
  out->reserved2 = s->reserved2;
}

void macho_sect_set(const MachoView *mv, uint8_t *sect, uint64_t addr,
                    uint32_t offset, uint32_t reloff) {
  struct section_64 *s = (struct section_64 *)sect;
  s->addr = addr;
  s->offset = offset;
  s->reloff = reloff;
}

void macho_sym_get(const MachoView *mv, const uint8_t *symbase, uint32_t idx,
                   MachoSym *out) {
  const struct nlist_64 *n = &((const struct nlist_64 *)symbase)[idx];
  out->n_strx = n->n_un.n_strx;
  out->n_type = n->n_type;
  out->n_sect = n->n_sect;
  out->n_desc = (int16_t)n->n_desc;
  out->n_value = n->n_value;
}

void macho_sym_set_value(const MachoView *mv, uint8_t *symbase, uint32_t idx,
                         uint64_t value) {
  ((struct nlist_64 *)symbase)[idx].n_value = value;
}

uint32_t macho_hdr_ncmds(const MachoView *mv, const uint8_t *hdr) {
  return ((const struct mach_header_64 *)hdr)->ncmds;
}

uint32_t macho_hdr_sizeofcmds(const MachoView *mv, const uint8_t *hdr) {
  return ((const struct mach_header_64 *)hdr)->sizeofcmds;
}

uint32_t macho_hdr_filetype(const MachoView *mv, const uint8_t *hdr) {
  return ((const struct mach_header_64 *)hdr)->filetype;
}

uint32_t macho_hdr_flags(const MachoView *mv, const uint8_t *hdr) {
  return ((const struct mach_header_64 *)hdr)->flags;
}

void macho_hdr_set_filetype(const MachoView *mv, uint8_t *hdr, uint32_t ft) {
  ((struct mach_header_64 *)hdr)->filetype = ft;
}

void macho_hdr_set_flags(const MachoView *mv, uint8_t *hdr, uint32_t fl) {
  ((struct mach_header_64 *)hdr)->flags = fl;
}

void macho_hdr_set_cmds(const MachoView *mv, uint8_t *hdr, uint32_t ncmds,
                        uint32_t sizeofcmds) {
  ((struct mach_header_64 *)hdr)->ncmds = ncmds;
  ((struct mach_header_64 *)hdr)->sizeofcmds = sizeofcmds;
}

uint32_t macho_emit_seg1(const MachoView *mv, uint8_t *dst, const char *segname,
                         uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                         uint64_t filesize, int32_t maxprot, int32_t initprot,
                         const char *sectname, uint64_t sect_addr,
                         uint64_t sect_size, uint32_t sect_off,
                         uint32_t sect_align) {
  uint32_t cmdsize = macho_seg1_cmdsize(mv);

  memset(dst, 0, cmdsize);
  struct segment_command_64 *sc = (struct segment_command_64 *)dst;
  sc->cmd = LC_SEGMENT_64;
  sc->cmdsize = cmdsize;
  memcpy(sc->segname, segname, strlen(segname));
  sc->vmaddr = vmaddr;
  sc->vmsize = vmsize;
  sc->fileoff = fileoff;
  sc->filesize = filesize;
  sc->maxprot = maxprot;
  sc->initprot = initprot;
  sc->nsects = 1;

  struct section_64 *st = (struct section_64 *)(sc + 1);
  memcpy(st->sectname, sectname, strlen(sectname));
  memcpy(st->segname, segname, strlen(segname));
  st->addr = sect_addr;
  st->size = sect_size;
  st->offset = sect_off;
  st->align = sect_align;


  return cmdsize;
}
