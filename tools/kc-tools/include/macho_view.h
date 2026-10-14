#pragma once
/*
 * A width-neutral view over a thin Mach-O image.
 */
#include <stddef.h>
#include <stdint.h>

#include "macho_fmt.h"

typedef struct {
  const uint8_t *base;     // start of the thin image (past any fat header)
  size_t size;             // bytes available from base
  int is64;
  int32_t cputype;
  int32_t cpusubtype;
  uint32_t filetype;
  uint32_t ncmds;
  uint32_t sizeofcmds;
  uint32_t flags;
  uint32_t hdrsize;        // sizeof(mach_header[_64])
  const uint8_t *lc0;      // first load command
} MachoView;

typedef struct {
  char segname[17];
  uint64_t vmaddr;
  uint64_t vmsize;
  uint64_t fileoff;
  uint64_t filesize;
  int32_t maxprot;
  int32_t initprot;
  uint32_t nsects;
  uint32_t flags;
  const uint8_t *sects;    // first section record, or NULL
} MachoSeg;

typedef struct {
  char sectname[17];
  char segname[17];
  uint64_t addr;
  uint64_t size;
  uint32_t offset;
  uint32_t align;
  uint32_t reloff;
  uint32_t nreloc;
  uint32_t flags;
  uint32_t reserved1;
  uint32_t reserved2;
} MachoSect;

typedef struct {
  uint32_t n_strx;
  uint8_t n_type;
  uint8_t n_sect;
  int16_t n_desc;
  uint64_t n_value;
} MachoSym;

// Parse a Mach-O (selecting a slice if it is fat). Returns 0 on success.
int macho_view(const uint8_t *data, size_t size, MachoView *mv);

// The load command that begins a segment, for this image's width.
static inline uint32_t macho_lc_segment(const MachoView *mv) {
  return mv->is64 ? LC_SEGMENT_64 : LC_SEGMENT;
}

static inline size_t macho_seg_cmdsize(const MachoView *mv) {
  return mv->is64 ? sizeof(struct segment_command_64)
                  : sizeof(struct segment_command);
}

static inline size_t macho_sect_size(const MachoView *mv) {
  return mv->is64 ? sizeof(struct section_64) : sizeof(struct section);
}

static inline size_t macho_nlist_size(const MachoView *mv) {
  return mv->is64 ? sizeof(struct nlist_64) : sizeof(struct nlist);
}

// Iterate load commands: `for (MACHO_EACH_LC(mv, lc))`.
#define MACHO_EACH_LC(mv, lc)                                                  \
  const uint8_t *lc = (mv)->lc0;                                               \
  lc && (size_t)(lc - (mv)->base) < (size_t)((mv)->hdrsize + (mv)->sizeofcmds); \
  lc += ((const struct load_command *)lc)->cmdsize

int macho_is_segment(const MachoView *mv, const uint8_t *lc);
void macho_seg_get(const MachoView *mv, const uint8_t *lc, MachoSeg *out);
void macho_seg_set_addrs(const MachoView *mv, uint8_t *lc, uint64_t vmaddr,
                         uint64_t vmsize, uint64_t fileoff, uint64_t filesize);
void macho_sect_get(const MachoView *mv, const uint8_t *sect, MachoSect *out);
void macho_sect_set(const MachoView *mv, uint8_t *sect, uint64_t addr,
                    uint32_t offset, uint32_t reloff);
void macho_sym_get(const MachoView *mv, const uint8_t *symbase, uint32_t idx,
                   MachoSym *out);
void macho_sym_set_value(const MachoView *mv, uint8_t *symbase, uint32_t idx,
                         uint64_t value);

// Header field access, for images being rewritten in place.
uint32_t macho_hdr_ncmds(const MachoView *mv, const uint8_t *hdr);
uint32_t macho_hdr_sizeofcmds(const MachoView *mv, const uint8_t *hdr);
uint32_t macho_hdr_filetype(const MachoView *mv, const uint8_t *hdr);
uint32_t macho_hdr_flags(const MachoView *mv, const uint8_t *hdr);
void macho_hdr_set_filetype(const MachoView *mv, uint8_t *hdr, uint32_t ft);
void macho_hdr_set_flags(const MachoView *mv, uint8_t *hdr, uint32_t fl);
void macho_hdr_set_cmds(const MachoView *mv, uint8_t *hdr, uint32_t ncmds,
                        uint32_t sizeofcmds);

// Size of a segment command carrying exactly one section.
static inline uint32_t macho_seg1_cmdsize(const MachoView *mv) {
  return (uint32_t)(macho_seg_cmdsize(mv) + macho_sect_size(mv));
}

// Emit a segment command plus its single section at dst. Returns the cmdsize.
uint32_t macho_emit_seg1(const MachoView *mv, uint8_t *dst, const char *segname,
                         uint64_t vmaddr, uint64_t vmsize, uint64_t fileoff,
                         uint64_t filesize, int32_t maxprot, int32_t initprot,
                         const char *sectname, uint64_t sect_addr,
                         uint64_t sect_size, uint32_t sect_off,
                         uint32_t sect_align);
