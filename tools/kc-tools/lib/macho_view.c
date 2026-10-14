#include <stdio.h>
#include <string.h>

#include "macho_view.h"

static uint32_t bswap32(uint32_t v) {
  return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v & 0xff0000u) >> 8) |
         ((v & 0xff000000u) >> 24);
}

static void copy_name(char *dst, const char *src, size_t n) {
  memcpy(dst, src, n);
  dst[n] = '\0';
}

int macho_view(const uint8_t *data, size_t size, MachoView *mv) {
  if (!data || size < sizeof(struct mach_header) || !mv) {
    return -1;
  }

  const uint8_t *s = data;
  size_t ssize = size;
  uint32_t magic = *(const uint32_t *)s;

  if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
    const struct fat_header *fh = (const struct fat_header *)data;
    uint32_t n = bswap32(fh->nfat_arch);
    const struct fat_arch *fa = (const struct fat_arch *)(data + sizeof(*fh));
    const struct fat_arch *pick = NULL;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t off = bswap32(fa[i].offset);
      uint32_t sz = bswap32(fa[i].size);
      if ((size_t)off + sz > size) {
        continue;
      }
      uint32_t m = *(const uint32_t *)(data + off);
      if (m == MH_MAGIC_64) {
        pick = &fa[i];
        break;
      }
      if (m == MH_MAGIC && !pick) {
        pick = &fa[i];
      }
    }
    if (!pick) {
      return -1;
    }
    s = data + bswap32(pick->offset);
    ssize = bswap32(pick->size);
    magic = *(const uint32_t *)s;
  }

  if (magic == MH_MAGIC_64) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)s;
    mv->is64 = 1;
    mv->hdrsize = (uint32_t)sizeof(*mh);
    mv->cputype = mh->cputype;
    mv->cpusubtype = mh->cpusubtype;
    mv->filetype = mh->filetype;
    mv->ncmds = mh->ncmds;
    mv->sizeofcmds = mh->sizeofcmds;
    mv->flags = mh->flags;
  } else if (magic == MH_MAGIC) {
    const struct mach_header *mh = (const struct mach_header *)s;
    mv->is64 = 0;
    mv->hdrsize = (uint32_t)sizeof(*mh);
    mv->cputype = mh->cputype;
    mv->cpusubtype = mh->cpusubtype;
    mv->filetype = mh->filetype;
    mv->ncmds = mh->ncmds;
    mv->sizeofcmds = mh->sizeofcmds;
    mv->flags = mh->flags;
  } else {
    return -1;
  }

  mv->base = s;
  mv->size = ssize;
  mv->lc0 = s + mv->hdrsize;
  return 0;
}

int macho_is_segment(const MachoView *mv, const uint8_t *lc) {
  return ((const struct load_command *)lc)->cmd == macho_lc_segment(mv);
}

void macho_seg_get(const MachoView *mv, const uint8_t *lc, MachoSeg *out) {
  if (mv->is64) {
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
  } else {
    const struct segment_command *sc = (const struct segment_command *)lc;
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
}

void macho_seg_set_addrs(const MachoView *mv, uint8_t *lc, uint64_t vmaddr,
                         uint64_t vmsize, uint64_t fileoff, uint64_t filesize) {
  if (mv->is64) {
    struct segment_command_64 *sc = (struct segment_command_64 *)lc;
    sc->vmaddr = vmaddr;
    sc->vmsize = vmsize;
    sc->fileoff = fileoff;
    sc->filesize = filesize;
  } else {
    struct segment_command *sc = (struct segment_command *)lc;
    sc->vmaddr = (uint32_t)vmaddr;
    sc->vmsize = (uint32_t)vmsize;
    sc->fileoff = (uint32_t)fileoff;
    sc->filesize = (uint32_t)filesize;
  }
}

void macho_sect_get(const MachoView *mv, const uint8_t *sect, MachoSect *out) {
  if (mv->is64) {
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
  } else {
    const struct section *s = (const struct section *)sect;
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
}

void macho_sect_set(const MachoView *mv, uint8_t *sect, uint64_t addr,
                    uint32_t offset, uint32_t reloff) {
  if (mv->is64) {
    struct section_64 *s = (struct section_64 *)sect;
    s->addr = addr;
    s->offset = offset;
    s->reloff = reloff;
  } else {
    struct section *s = (struct section *)sect;
    s->addr = (uint32_t)addr;
    s->offset = offset;
    s->reloff = reloff;
  }
}

void macho_sym_get(const MachoView *mv, const uint8_t *symbase, uint32_t idx,
                   MachoSym *out) {
  if (mv->is64) {
    const struct nlist_64 *n = &((const struct nlist_64 *)symbase)[idx];
    out->n_strx = n->n_un.n_strx;
    out->n_type = n->n_type;
    out->n_sect = n->n_sect;
    out->n_desc = (int16_t)n->n_desc;
    out->n_value = n->n_value;
  } else {
    const struct nlist *n = &((const struct nlist *)symbase)[idx];
    out->n_strx = n->n_un.n_strx;
    out->n_type = n->n_type;
    out->n_sect = n->n_sect;
    out->n_desc = n->n_desc;
    out->n_value = n->n_value;
  }
}

void macho_sym_set_value(const MachoView *mv, uint8_t *symbase, uint32_t idx,
                         uint64_t value) {
  if (mv->is64) {
    ((struct nlist_64 *)symbase)[idx].n_value = value;
  } else {
    ((struct nlist *)symbase)[idx].n_value = (uint32_t)value;
  }
}

uint32_t macho_hdr_ncmds(const MachoView *mv, const uint8_t *hdr) {
  return mv->is64 ? ((const struct mach_header_64 *)hdr)->ncmds
                  : ((const struct mach_header *)hdr)->ncmds;
}

uint32_t macho_hdr_sizeofcmds(const MachoView *mv, const uint8_t *hdr) {
  return mv->is64 ? ((const struct mach_header_64 *)hdr)->sizeofcmds
                  : ((const struct mach_header *)hdr)->sizeofcmds;
}

uint32_t macho_hdr_filetype(const MachoView *mv, const uint8_t *hdr) {
  return mv->is64 ? ((const struct mach_header_64 *)hdr)->filetype
                  : ((const struct mach_header *)hdr)->filetype;
}

uint32_t macho_hdr_flags(const MachoView *mv, const uint8_t *hdr) {
  return mv->is64 ? ((const struct mach_header_64 *)hdr)->flags
                  : ((const struct mach_header *)hdr)->flags;
}

void macho_hdr_set_filetype(const MachoView *mv, uint8_t *hdr, uint32_t ft) {
  if (mv->is64) {
    ((struct mach_header_64 *)hdr)->filetype = ft;
  } else {
    ((struct mach_header *)hdr)->filetype = ft;
  }
}

void macho_hdr_set_flags(const MachoView *mv, uint8_t *hdr, uint32_t fl) {
  if (mv->is64) {
    ((struct mach_header_64 *)hdr)->flags = fl;
  } else {
    ((struct mach_header *)hdr)->flags = fl;
  }
}

void macho_hdr_set_cmds(const MachoView *mv, uint8_t *hdr, uint32_t ncmds,
                        uint32_t sizeofcmds) {
  if (mv->is64) {
    ((struct mach_header_64 *)hdr)->ncmds = ncmds;
    ((struct mach_header_64 *)hdr)->sizeofcmds = sizeofcmds;
  } else {
    ((struct mach_header *)hdr)->ncmds = ncmds;
    ((struct mach_header *)hdr)->sizeofcmds = sizeofcmds;
  }
}

uint32_t macho_emit_seg1(const MachoView *mv, uint8_t *dst, const char *segname,
                         uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                         uint64_t filesize, int32_t maxprot, int32_t initprot,
                         const char *sectname, uint64_t sect_addr,
                         uint64_t sect_size, uint32_t sect_off,
                         uint32_t sect_align) {
  uint32_t cmdsize = macho_seg1_cmdsize(mv);

  memset(dst, 0, cmdsize);
  if (mv->is64) {
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
  } else {
    struct segment_command *sc = (struct segment_command *)dst;
    sc->cmd = LC_SEGMENT;
    sc->cmdsize = cmdsize;
    memcpy(sc->segname, segname, strlen(segname));
    sc->vmaddr = (uint32_t)vmaddr;
    sc->vmsize = (uint32_t)vmsize;
    sc->fileoff = (uint32_t)fileoff;
    sc->filesize = (uint32_t)filesize;
    sc->maxprot = maxprot;
    sc->initprot = initprot;
    sc->nsects = 1;

    struct section *st = (struct section *)(sc + 1);
    memcpy(st->sectname, sectname, strlen(sectname));
    memcpy(st->segname, segname, strlen(segname));
    st->addr = (uint32_t)sect_addr;
    st->size = (uint32_t)sect_size;
    st->offset = sect_off;
    st->align = sect_align;
  }
  return cmdsize;
}
