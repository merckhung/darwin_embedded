#pragma once
#include <stdint.h>

// Magic numbers
#define MH_MAGIC_64     0xfeedfacf
#define MH_CIGAM_64     0xcffaedfe
#define MH_MAGIC        0xfeedface
#define FAT_MAGIC       0xcafebabe
#define FAT_CIGAM       0xbebafeca

// File types
#define MH_EXECUTE      0x2
#define MH_DYLIB        0x6
#define MH_BUNDLE       0x8
#define MH_KEXT_BUNDLE  0xb

// CPU types
#define CPU_TYPE_X86_64 ((int)(7 | 0x01000000))
#define CPU_TYPE_ARM64  ((int)(12 | 0x01000000))
#define CPU_SUBTYPE_ALL 0

// Flags
#define MH_NOUNDEFS       0x1
#define MH_PIE            0x200000
#define MH_DYLIB_IN_CACHE 0x80000000u  /* mach-o is part of a fileset/cache */

// Load commands
#define LC_REQ_DYLD           0x80000000u
#define LC_SEGMENT            0x1
#define LC_SEGMENT_64         0x19
#define LC_SYMTAB             0x2
#define LC_DYSYMTAB           0xb
#define LC_UUID               0x1b
#define LC_UNIXTHREAD         0x5
#define LC_VERSION_MIN_MACOSX 0x24
/* 0x2A. Was 0x42 here, which is not a load command at all; nothing read it
 * until the collection-header split needed to classify every command. */
#define LC_SOURCE_VERSION     0x2a
#define LC_BUILD_VERSION      0x32
#define LC_FUNCTION_STARTS    0x26
#define LC_DATA_IN_CODE       0x29
// LC_DYLD_INFO / struct dyld_info_command moved into ../kc-tools/shared/linker.c
// (its only consumer), so the shared linker stays self-contained.
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)
#define LC_FILESET_ENTRY      (0x35 | LC_REQ_DYLD)

// MH_FILESET filetype
#ifndef MH_FILESET
#define MH_FILESET            0xc
#endif

// x86_64 thread state (for LC_UNIXTHREAD entry)
#define X86_THREAD_STATE64     4
#define X86_THREAD_STATE64_CNT 42   // uint32 count

// LC_FILESET_ENTRY payload
struct fileset_entry_command {
  uint32_t cmd;        // LC_FILESET_ENTRY
  uint32_t cmdsize;
  uint64_t vmaddr;     // memory address of the entry's mach header
  uint64_t fileoff;    // file offset of the entry's mach header
  uint32_t entry_id;   // offset (from cmd start) to the entry id C-string
  uint32_t reserved;
};

// LC_DYLD_CHAINED_FIXUPS / linkedit_data_command
struct linkedit_data_command {
  uint32_t cmd;
  uint32_t cmdsize;
  uint32_t dataoff;
  uint32_t datasize;
};

// dyld_chained_fixups_header (version 0, all-rebase, imports_count 0)
struct dyld_chained_fixups_header {
  uint32_t fixups_version;  // 0
  uint32_t starts_offset;   // offset to dyld_chained_starts_in_image
  uint32_t imports_offset;
  uint32_t symbols_offset;
  uint32_t imports_count;   // 0
  uint32_t imports_format;  // 1
  uint32_t symbols_format;  // 0
};

// vm_prot
#define VM_PROT_NONE    0
#define VM_PROT_READ    1
#define VM_PROT_WRITE   2
#define VM_PROT_EXECUTE 4

// Section types
#define S_REGULAR                  0x0
#define S_ZEROFILL                 0x1
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS     0x7
#define S_SYMBOL_STUBS             0x8
#define S_MOD_INIT_FUNC_POINTERS   0x9
#define S_MOD_TERM_FUNC_POINTERS   0xa
#define SECTION_TYPE               0xff

// nlist flags
#define N_STAB  0xe0    /* mask: any bit set means a debug (stab) entry */
#define N_UNDF  0x0
#define N_ABS   0x2
#define N_SECT  0xe
#define N_TYPE  0xe
#define N_EXT   0x01
#define N_PEXT  0x10
#define NO_SECT 0

// relocation_info r_length values
#define RL_BYTE  0
#define RL_SHORT 1
#define RL_LONG  2
#define RL_QUAD  3

// x86_64 relocation types
#define X86_64_RELOC_UNSIGNED   0
#define X86_64_RELOC_SIGNED     1
#define X86_64_RELOC_BRANCH     2
#define X86_64_RELOC_GOT_LOAD   3
#define X86_64_RELOC_GOT        4
#define X86_64_RELOC_SUBTRACTOR 5
#define X86_64_RELOC_SIGNED_1   6
#define X86_64_RELOC_SIGNED_2   7
#define X86_64_RELOC_SIGNED_4   8
#define X86_64_RELOC_TLV        9

struct mach_header_64 {
  uint32_t magic;
  int32_t cputype;
  int32_t cpusubtype;
  uint32_t filetype;
  uint32_t ncmds;
  uint32_t sizeofcmds;
  uint32_t flags;
  uint32_t reserved;
};

struct load_command {
  uint32_t cmd;
  uint32_t cmdsize;
};

struct segment_command_64 {
  uint32_t cmd;
  uint32_t cmdsize;
  char segname[16];
  uint64_t vmaddr;
  uint64_t vmsize;
  uint64_t fileoff;
  uint64_t filesize;
  int32_t maxprot;
  int32_t initprot;
  uint32_t nsects;
  uint32_t flags;
};

struct section_64 {
  char sectname[16];
  char segname[16];
  uint64_t addr;
  uint64_t size;
  uint32_t offset;
  uint32_t align;
  uint32_t reloff;
  uint32_t nreloc;
  uint32_t flags;
  uint32_t reserved1;
  uint32_t reserved2;
  uint32_t reserved3;
};

struct symtab_command {
  uint32_t cmd;
  uint32_t cmdsize;
  uint32_t symoff;
  uint32_t nsyms;
  uint32_t stroff;
  uint32_t strsize;
};

struct dysymtab_command {
  uint32_t cmd;
  uint32_t cmdsize;
  uint32_t ilocalsym;
  uint32_t nlocalsym;
  uint32_t iextdefsym;
  uint32_t nextdefsym;
  uint32_t iundefsym;
  uint32_t nundefsym;
  uint32_t tocoff;
  uint32_t ntoc;
  uint32_t modtaboff;
  uint32_t nmodtab;
  uint32_t extrefsymoff;
  uint32_t nextrefsyms;
  uint32_t indirectsymoff;
  uint32_t nindirectsyms;
  uint32_t extreloff;
  uint32_t nextrel;
  uint32_t locreloff;
  uint32_t nlocrel;
};

struct nlist_64 {
  union { uint32_t n_strx; } n_un;
  uint8_t n_type;
  uint8_t n_sect;
  uint16_t n_desc;
  uint64_t n_value;
};

struct relocation_info {
  int32_t r_address;
  uint32_t r_symbolnum : 24,
           r_pcrel     : 1,
           r_length    : 2,
           r_extern    : 1,
           r_type      : 4;
};

// 32-bit Mach-O

struct mach_header {
  uint32_t magic;
  int32_t cputype;
  int32_t cpusubtype;
  uint32_t filetype;
  uint32_t ncmds;
  uint32_t sizeofcmds;
  uint32_t flags;
};

struct segment_command {
  uint32_t cmd;
  uint32_t cmdsize;
  char segname[16];
  uint32_t vmaddr;
  uint32_t vmsize;
  uint32_t fileoff;
  uint32_t filesize;
  int32_t maxprot;
  int32_t initprot;
  uint32_t nsects;
  uint32_t flags;
};

struct section {
  char sectname[16];
  char segname[16];
  uint32_t addr;
  uint32_t size;
  uint32_t offset;
  uint32_t align;
  uint32_t reloff;
  uint32_t nreloc;
  uint32_t flags;
  uint32_t reserved1;
  uint32_t reserved2;
};

struct nlist {
  union { uint32_t n_strx; } n_un;
  uint8_t n_type;
  uint8_t n_sect;
  int16_t n_desc;
  uint32_t n_value;
};

struct fat_header {
  uint32_t magic;
  uint32_t nfat_arch;
};

struct fat_arch {
  int32_t cputype;
  int32_t cpusubtype;
  uint32_t offset;
  uint32_t size;
  uint32_t align;
};
