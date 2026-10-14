#ifndef _STUFF_BYTESEX_H_
#define _STUFF_BYTESEX_H_

#if defined(__MWERKS__) && !defined(__MACH__)
#include <s_types.h>
#else
#include <sys/types.h>
#endif
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/m68k/thread_status.h>
#include <mach/ppc/thread_status.h>
#include <mach/m88k/thread_status.h>
#include <mach/i860/thread_status.h>
#include <mach/i386/thread_status.h>
#include <mach/hppa/thread_status.h>
#include <mach/sparc/thread_status.h>
#undef VALID_THREAD_STATE_FLAVOR
#include <mach/arm/thread_status.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <mach-o/ranlib.h>
#include "stuff/thread_states.h"
#include "../include/xar/xar.h"
#include "stuff/bool.h"

#define SWAP_SHORT(a) ( ((a & 0xff00) >> 8) | ((a & 0x00ff) << 8) )
#define SWAP_INT(a) ( ((a & 0xff000000) >> 24) | ((a & 0x00ff0000) >> 8) | ((a & 0x0000ff00) << 8) | ((a & 0x000000ff) << 24) )
__private_extern__ long long SWAP_LONG_LONG(long long ll);

enum byte_sex {
    UNKNOWN_BYTE_SEX,
    BIG_ENDIAN_BYTE_SEX,
    LITTLE_ENDIAN_BYTE_SEX
};

struct fvmaddr_command;
struct router_command;

__private_extern__ enum byte_sex get_host_byte_sex(void);

__private_extern__ void swap_fat_header(
    struct fat_header *fat_header,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_fat_arch(
    struct fat_arch *fat_archs,
    uint32_t nfat_arch,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_fat_arch_64(
    struct fat_arch_64 *fat_archs64,
    uint32_t nfat_arch,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_mach_header(
    struct mach_header *mh,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_mach_header_64(
    struct mach_header_64 *mh64,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_load_command(
    struct load_command *lc,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_segment_command(
    struct segment_command *sg,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_segment_command_64(
    struct segment_command_64 *sg64,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_section(
    struct section *s,
    uint32_t nsects,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_section_64(
    struct section_64 *s64,
    uint32_t nsects,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_symtab_command(
    struct symtab_command *st,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dysymtab_command(
    struct dysymtab_command *dyst,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_symseg_command(
    struct symseg_command *ss,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ident_command(
    struct ident_command *id,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_fvmaddr_command(
    struct fvmaddr_command *fvmaddr,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_fvmlib_command(
    struct fvmlib_command *fl,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylib_command(
    struct dylib_command *dl,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sub_framework_command(
    struct sub_framework_command *sub,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sub_client_command(
    struct sub_client_command *sub,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sub_umbrella_command(
    struct sub_umbrella_command *sub,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sub_library_command(
    struct sub_library_command *sub,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_prebound_dylib_command(
    struct prebound_dylib_command *pbdl,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylinker_command(
    struct dylinker_command *dyld,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_fvmfile_command(
    struct fvmfile_command *ff,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_thread_command(
    struct thread_command *ut,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m68k_thread_state_regs(
    struct m68k_thread_state_regs *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m68k_thread_state_68882(
    struct m68k_thread_state_68882 *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m68k_thread_state_user_reg(
    struct m68k_thread_state_user_reg *user_reg,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ppc_thread_state_t(
    ppc_thread_state_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ppc_thread_state64_t(
    ppc_thread_state64_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ppc_float_state_t(
    ppc_float_state_t *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ppc_exception_state_t(
    ppc_exception_state_t *state,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m88k_thread_state_grf_t(
    m88k_thread_state_grf_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m88k_thread_state_xrf_t(
    m88k_thread_state_xrf_t *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m88k_thread_state_user_t(
    m88k_thread_state_user_t *user,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_m88110_thread_state_impl_t(
    m88110_thread_state_impl_t *spu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_i860_thread_state_regs(
    struct i860_thread_state_regs *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_i386_thread_state(
    i386_thread_state_t *cpu,
    enum byte_sex target_byte_sex);

/* current i386 thread states */
#if i386_THREAD_STATE == 1
__private_extern__ void swap_i386_float_state(
    struct __darwin_i386_float_state *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_i386_exception_state(
    struct __darwin_i386_exception_state *except,
    enum byte_sex target_byte_sex);
#endif

/* i386 thread states in 10.4 and earlier */
#if i386_THREAD_STATE == -1
__private_extern__ void swap_i386_thread_fpstate(
    i386_thread_fpstate_t *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_i386_thread_exceptstate(
    i386_thread_exceptstate_t *except,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_i386_thread_cthreadstate(
    i386_thread_cthreadstate_t *user,
    enum byte_sex target_byte_sex);
#endif

__private_extern__ void swap_x86_thread_state64(
    x86_thread_state64_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_x86_float_state64(
    x86_float_state64_t *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_x86_exception_state64(
    x86_exception_state64_t *except,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_x86_debug_state32(
    x86_debug_state32_t *debug,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_x86_debug_state64(
    x86_debug_state64_t *debug,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_hppa_integer_thread_state(
    struct hp_pa_integer_thread_state *regs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_hppa_frame_thread_state(
    struct hp_pa_frame_thread_state *regs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_hppa_fp_thread_state(
    struct hp_pa_fp_thread_state *regs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sparc_thread_state_regs(
    struct sparc_thread_state_regs *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_sparc_thread_state_fpu(
    struct sparc_thread_state_fpu *fpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_arm_thread_state_t(
    arm_thread_state_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_arm_thread_state64_t(
    arm_thread_state64_t *cpu,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_arm_exception_state64_t(
    arm_exception_state64_t *except,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_nlist(
    struct nlist *symbols,
    uint32_t nsymbols,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_nlist_64(
    struct nlist_64 *symbols64,
    uint32_t nsymbols,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ranlib(
    struct ranlib *ranlibs,
    uint32_t nranlibs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_ranlib_64(
    struct ranlib_64 *ranlibs,
    uint64_t nranlibs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_relocation_info(
    struct relocation_info *relocs,
    uint32_t nrelocs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_indirect_symbols(
    uint32_t *indirect_symbols,
    uint32_t nindirect_symbols,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylib_reference(
    struct dylib_reference *refs,
    uint32_t nrefs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylib_module(
    struct dylib_module *mods,
    uint32_t nmods,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylib_module_64(
    struct dylib_module_64 *mods,
    uint32_t nmods,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dylib_table_of_contents(
    struct dylib_table_of_contents *tocs,
    uint32_t ntocs,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_router_command(
    struct router_command *router,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_twolevel_hints_command(
    struct twolevel_hints_command *hints_cmd,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_twolevel_hint(
    struct twolevel_hint *hints,
    uint32_t nhints,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_prebind_cksum_command(
    struct prebind_cksum_command *cksum_cmd,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_uuid_command(
    struct uuid_command *uuid_cmd,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_linkedit_data_command(
    struct linkedit_data_command *ld,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_version_min_command(
    struct version_min_command *ver_min,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_rpath_command(
    struct rpath_command *rpath,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_encryption_command(
    struct encryption_info_command *ec,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_encryption_command_64(
    struct encryption_info_command_64 *ec,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_linker_option_command(
    struct linker_option_command *lo,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_dyld_info_command(
    struct dyld_info_command *ed,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_entry_point_command(
    struct entry_point_command *ep,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_source_version_command(
    struct source_version_command *sv,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_build_version_command(
    struct build_version_command *bv,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_build_tool_version(
    struct build_tool_version *btv,
    uint32_t ntools,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_note_command(
    struct note_command *nc,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_data_in_code_entry(
    struct data_in_code_entry *dices,
    uint32_t ndices,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_trie_children(
    uint8_t *start,
    uint8_t *end,
    enum byte_sex target_byte_sex);

__private_extern__ void swap_xar_header(
    struct xar_header *xar,
    enum byte_sex target_byte_sex);

__private_extern__ enum bool swap_object_headers(
    void *mach_header,
    struct load_command *load_commands);

#endif /* _STUFF_BYTESEX_H_ */
