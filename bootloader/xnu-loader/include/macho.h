#ifndef MACHO_H
#define MACHO_H

#include "common.h"
#include "app.h"
#include <stdint.h>

#define MH_MAGIC_64   0xfeedfacf
#define MH_EXECUTE    0x2
#define MH_FILESET    0xc          /* kernel collection (KC) */
#define LC_SEGMENT_64 0x19
#define LC_UNIXTHREAD 0x5
#define LC_MAIN       0x80000028
#define LC_DYSYMTAB   0xb

#define KASLR_SLIDE_GRANULE 0x200000ULL   /* 2 MB per slide unit */

#define MACHO_MAX_SEGMENTS 256
#define EFI_PAGE_SHIFT 12

#define X86_THREAD_STATE64 4

typedef struct macho_header_64 {
  UINT32 magic;
  INT32 cputype;
  INT32 cpusubtype;
  UINT32 filetype;
  UINT32 ncmds;
  UINT32 sizeofcmds;
  UINT32 flags;
  UINT32 reserved;
} macho_header_64;

typedef struct macho_load_command {
  UINT32 cmd;
  UINT32 cmdsize;
} macho_load_command;

typedef struct macho_segment_command_64 {
  UINT32 cmd;
  UINT32 cmdsize;
  CHAR8 segname[16];
  UINT64 vmaddr;
  UINT64 vmsize;
  UINT64 fileoff;
  UINT64 filesize;
  INT32 maxprot;
  INT32 initprot;
  UINT32 nsects;
  UINT32 flags;
} macho_segment_command_64;

typedef struct macho_section_64 {
  CHAR8 sectname[16];
  CHAR8 segname[16];
  UINT64 addr;
  UINT64 size;
  UINT32 offset;
  UINT32 align;
  UINT32 reloff;
  UINT32 nreloc;
  UINT32 flags;
  UINT32 reserved1;
  UINT32 reserved2;
  UINT32 reserved3;
} macho_section_64;

typedef struct macho_thread_command {
  UINT32 cmd;
  UINT32 cmdsize;
} macho_thread_command;

typedef struct x86_thread_state64 {
  UINT64 rax;
  UINT64 rbx;
  UINT64 rcx;
  UINT64 rdx;
  UINT64 rdi;
  UINT64 rsi;
  UINT64 rbp;
  UINT64 rsp;
  UINT64 r8;
  UINT64 r9;
  UINT64 r10;
  UINT64 r11;
  UINT64 r12;
  UINT64 r13;
  UINT64 r14;
  UINT64 r15;
  UINT64 rip;
  UINT64 rflags;
  UINT64 cs;
  UINT64 fs;
  UINT64 gs;
} x86_thread_state64;

#define ARM_THREAD_STATE64 6

typedef struct arm_thread_state64 {
  UINT64 x[29]; /* x0-x28 */
  UINT64 fp;    /* x29 */
  UINT64 lr;    /* x30 */
  UINT64 sp;    /* x31 */
  UINT64 pc;
  UINT32 cpsr;
  UINT32 flags;
} arm_thread_state64;

// riscv64 as ld64 numbers it, 24 with the 64-bit abi bit, flavor 1 is x0..x31 then pc
#define CPU_TYPE_RISCV64 (24 | 0x01000000)
#define RISCV_THREAD_STATE64 1

typedef struct riscv_thread_state64 {
  UINT64 x[32];
  UINT64 pc;
} riscv_thread_state64;

typedef struct macho_dysymtab_command {
  UINT32 cmd;
  UINT32 cmdsize;
  UINT32 ilocalsym;
  UINT32 nlocalsym;
  UINT32 iextdefsym;
  UINT32 nextdefsym;
  UINT32 iundefsym;
  UINT32 nundefsym;
  UINT32 tocoff;
  UINT32 ntoc;
  UINT32 modtaboff;
  UINT32 nmodtab;
  UINT32 extrefsymoff;
  UINT32 nextrefsyms;
  UINT32 indirectsymoff;
  UINT32 nindirectsyms;
  UINT32 extreloff;
  UINT32 nextrel;
  UINT32 locreloff;
  UINT32 nlocrel;
} macho_dysymtab_command;

typedef struct macho_relocation_info {
  INT32  r_address;
  UINT32 r_symbolnum : 24;
  UINT32 r_pcrel     : 1;
  UINT32 r_length    : 2;
  UINT32 r_extern    : 1;
  UINT32 r_type      : 4;
} macho_relocation_info;

typedef struct MachoImage {
  VOID *data;
  UINTN size;
  macho_header_64 *header;
} MachoImage;

typedef struct MachoLoadedSegment {
  CHAR8 name[17];
  UINT64 vmaddr;
  UINT64 vmsize;
  UINT64 fileoff;
  UINT64 filesize;
  VOID *host_addr;
  UINTN page_count;
  UINT32 phys_target;
  UINT32 initprot;
} MachoLoadedSegment;

typedef struct MachoLoadResult {
  EFI_PHYSICAL_ADDRESS host_base;
  UINT64 lowest_vmaddr;
  UINT64 highest_vmaddr;
  UINT64 image_size;

  UINTN segment_count;
  MachoLoadedSegment segments[MACHO_MAX_SEGMENTS];
} MachoLoadResult;

EFI_STATUS macho_parse(
    VOID *data,
    UINTN size,
    MachoImage *out_image);

EFI_STATUS macho_dump(
    MachoImage *image);

EFI_STATUS macho_load_segments(
    AppContext *ctx,
    MachoImage *image,
    MachoLoadResult *out_result);

VOID macho_unload_segments(
    AppContext *ctx,
    MachoLoadResult *result);

EFI_STATUS macho_load_segments_contiguous(
    AppContext           *ctx,
    MachoImage           *image,
    EFI_PHYSICAL_ADDRESS  phys_base,
    MachoLoadResult      *out_result);

EFI_STATUS macho_find_entry_vmaddr(
    MachoImage *image,
    UINT64 *out_entry_vmaddr);

EFI_STATUS macho_compute_host_entry(
    MachoLoadResult *load_result,
    UINT64 entry_vmaddr,
    VOID **out_host_entry);

EFI_STATUS macho_find_segment_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg);

EFI_STATUS macho_find_section_for_vmaddr(
    MachoImage *image,
    UINT64 vmaddr,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec);

VOID macho_log_entry_context(
    MachoImage *image,
    UINT64 entry_vmaddr);

VOID macho_dump_entry_bytes(
    VOID *host_entry,
    UINTN count);

EFI_STATUS macho_find_section_by_name(
    MachoImage *image,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    macho_segment_command_64 **out_seg,
    macho_section_64 **out_sec);

EFI_STATUS macho_compute_host_address(
    MachoLoadResult *load_result,
    UINT64 vmaddr,
    VOID **out_host_addr);

EFI_STATUS macho_get_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname,
    UINT64 *out_vmaddr,
    UINT64 *out_size,
    VOID **out_host_addr);

/* Apply KASLR slide to staged segments.
 * Walks LC_DYSYMTAB local relocs in staging, adds kslide to each absolute
 * 64-bit pointer, then increments every segment's phys_target by kslide.
 * Must be called before ExitBootServices while staging memory is accessible. */
EFI_STATUS macho_apply_kaslr_slide(
    MachoImage      *image,
    MachoLoadResult *load_result,
    UINT32           kslide);

/* Patch the kernel's EMBEDDED Mach-O header (in __TEXT) by +kslide: every
 * LC_SEGMENT_64 vmaddr and section addr.  XNU rebuilds idlepml4 / the double
 * map / pmap from getsegbynamefromheader() using low32(vmaddr) as the physical
 * frame, so the header must reflect the slid layout or the new page tables map
 * the wrong physical pages (fault on the descriptor tables at the CR3 switch).
 * boot.efi does exactly this while loading __TEXT.  Operates on staging. */
EFI_STATUS macho_patch_header_slide(
    MachoImage      *image,
    MachoLoadResult *load_result,
    UINT32           kslide);

EFI_STATUS macho_compute_vm_range_pub(
    MachoImage *image,
    UINT64     *out_lowest,
    UINT64     *out_highest);

VOID macho_unload_contiguous(
    AppContext       *ctx,
    MachoLoadResult  *result);

VOID macho_log_section_host_info(
    MachoImage *image,
    MachoLoadResult *load_result,
    const CHAR8 *segname,
    const CHAR8 *sectname);

#endif