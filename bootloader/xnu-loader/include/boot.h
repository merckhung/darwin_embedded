#ifndef BOOT_H
#define BOOT_H

#include "common.h"
#include "macho.h"
#include "lowmem.h"
#include "devtree.h"
#include "../../video/metadata.h"

#define BOOT_LINE_LENGTH        1024
#define BOOT_STRING_LEN         BOOT_LINE_LENGTH

struct Boot_VideoV1 {
  uint32_t        v_baseAddr;
  uint32_t        v_display;
  uint32_t        v_rowBytes;
  uint32_t        v_width;
  uint32_t        v_height;
  uint32_t        v_depth;
};
typedef struct Boot_VideoV1 Boot_VideoV1;

struct Boot_Video {
  uint32_t        v_display;
  uint32_t        v_rowBytes;
  uint32_t        v_width;
  uint32_t        v_height;
  uint32_t        v_depth;
  uint8_t         v_rotate;
  uint8_t         v_resv_byte[3];
  uint32_t        v_resv[6];
  uint64_t        v_baseAddr;
};
typedef struct Boot_Video Boot_Video;

#define GRAPHICS_MODE         1
#define FB_TEXT_MODE          2

/* Struct describing an image passed in by the booter */
struct boot_icon_element {
	unsigned int    width;
	unsigned int    height;
	int             y_offset_from_center;
	unsigned int    data_size;
	unsigned int    __reserved1[4];
	unsigned char   data[0];
};
typedef struct boot_icon_element boot_icon_element;

/* Boot argument structure - passed into Mach kernel at boot time.
 * "Revision" can be incremented for compatible changes
 */
#define kBootArgsRevision               0
#define kBootArgsRevision0              kBootArgsRevision
#define kBootArgsRevision1              1 /* added KC_hdrs_addr */
#define kBootArgsVersion                2

/* Snapshot constants of previous revisions that are supported */
#define kBootArgsVersion1               1
#define kBootArgsVersion2               2
#define kBootArgsRevision2_0            0

#define kBootArgsEfiMode32              32
#define kBootArgsEfiMode64              64

/* Bitfields for boot_args->flags */
#define kBootArgsFlagRebootOnPanic      (1 << 0)
#define kBootArgsFlagHiDPI              (1 << 1)
#define kBootArgsFlagBlack              (1 << 2)
#define kBootArgsFlagCSRActiveConfig    (1 << 3)
#define kBootArgsFlagCSRConfigMode      (1 << 4)
#define kBootArgsFlagCSRBoot            (1 << 5)
#define kBootArgsFlagBlackBg            (1 << 6)
#define kBootArgsFlagLoginUI            (1 << 7)
#define kBootArgsFlagInstallUI          (1 << 8)
#define kBootArgsFlagRecoveryBoot       (1 << 10)

typedef struct boot_args {
  uint16_t Revision;
  uint16_t Version;

  uint8_t  efiMode;
  uint8_t  debugMode;
  uint16_t flags;

  char CommandLine[BOOT_LINE_LENGTH];

  uint32_t MemoryMap;
  uint32_t MemoryMapSize;
  uint32_t MemoryMapDescriptorSize;
  uint32_t MemoryMapDescriptorVersion;

  Boot_VideoV1 VideoV1;

  uint32_t deviceTreeP;
  uint32_t deviceTreeLength;

  uint32_t kaddr;
  uint32_t ksize;

  uint32_t efiRuntimeServicesPageStart;
  uint32_t efiRuntimeServicesPageCount;
  uint64_t efiRuntimeServicesVirtualPageStart;

  uint32_t efiSystemTable;
  uint32_t kslide;

  uint32_t performanceDataStart;
  uint32_t performanceDataSize;

  uint32_t keyStoreDataStart;
  uint32_t keyStoreDataSize;

  uint64_t bootMemStart;
  uint64_t bootMemSize;
  uint64_t PhysicalMemorySize;

  uint64_t FSBFrequency;
  uint64_t pciConfigSpaceBaseAddress;

  uint32_t pciConfigSpaceStartBusNumber;
  uint32_t pciConfigSpaceEndBusNumber;

  uint32_t csrActiveConfig;
  uint32_t csrCapabilities;
  uint32_t boot_SMC_plimit;

  uint16_t bootProgressMeterStart;
  uint16_t bootProgressMeterEnd;

  Boot_Video Video;

  uint32_t apfsDataStart;
  uint32_t apfsDataSize;

  uint64_t KC_hdrs_vaddr;

  uint64_t arvRootHashStart;
  uint64_t arvRootHashSize;

  uint64_t arvManifestStart;
  uint64_t arvManifestSize;

  uint64_t bsARVRootHashStart;
  uint64_t bsARVRootHashSize;

  uint64_t bsARVManifestStart;
  uint64_t bsARVManifestSize;

  uint32_t __reserved4[692];
} boot_args;

// riscv64 boot_args has this same layout (pexpert/pexpert/riscv/boot.h)
#if defined(__aarch64__) || defined(__riscv)
/* pexpert/arm64/boot.h's real Boot_Video: six plain unsigned longs, in
 * this exact order (baseAddr first) - NOT the same struct as the
 * extended x86 Boot_Video above (different field order, extra
 * rotate/reserved fields). */
typedef struct arm64_Boot_Video {
  uint64_t v_baseAddr;
  uint64_t v_display;
  uint64_t v_rowBytes;
  uint64_t v_width;
  uint64_t v_height;
  uint64_t v_depth;
} arm64_Boot_Video;

typedef struct arm64_boot_args {
  uint16_t Revision;
  uint16_t Version;
  uint64_t virtBase;
  uint64_t physBase;
  uint64_t memSize;
  uint64_t topOfKernelData;
  arm64_Boot_Video Video;
  uint32_t machineType;
  uint64_t deviceTreeP;      /* real field is a pointer; kept as a fixed-width
                              * physical address here since we're 64-bit either way */
  uint32_t deviceTreeLength;
  char CommandLine[BOOT_LINE_LENGTH];
  uint64_t bootFlags;
  uint64_t memSizeActual;
} arm64_boot_args;

#define ARM64_BOOT_ARGS_REVISION 2 /* kBootArgsRevision2 */
#define ARM64_BOOT_ARGS_VERSION  2 /* kBootArgsVersion2 */

EFI_STATUS arm64_boot_build_args(
    AppContext *ctx,
    const CHAR8 *cmdline,
    UINT64 virt_base,
    UINT64 phys_base,
    UINT64 mem_size,
    UINT64 top_of_kernel_data,
    UINT64 device_tree_phys,
    UINT32 device_tree_len,
    LowMemBuffer *out_args_buf,
    arm64_boot_args **out_args);

VOID arm64_boot_log_args(arm64_boot_args *args);
#endif /* __aarch64__ */

typedef struct BootArgsState {
  boot_args *args;
  LowMemBuffer args_buf;

  VOID *device_tree;
  UINT32 device_tree_size;
  LowMemBuffer device_tree_buf;

  EFI_MEMORY_DESCRIPTOR *memory_map;
  UINTN memory_map_size;
  UINTN memory_map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
  LowMemBuffer memory_map_buf;

  UINT64 rt_table_phys;
} BootArgsState;

EFI_STATUS boot_collect_memory_map(AppContext *ctx, BootArgsState *state);

EFI_STATUS boot_refresh_memory_map(AppContext *ctx, BootArgsState *state);

VOID boot_update_args_memory_map(AppContext *ctx, BootArgsState *state);

EFI_STATUS boot_set_command_line(BootArgsState *state,  boot_args *args, const CHAR8 *cmdline);
/* csr-active-config=0x... from the command line, if present. */
BOOLEAN boot_cmdline_csr_config(const CHAR8 *cmdline, UINT32 *out);

EFI_STATUS boot_build_args(AppContext *ctx, const CHAR8 *cmdline, MachoLoadResult *load_result, BootArgsState *state);

/* Result of probing the firmware's GOP, in the widest form either boot_args
 * flavour needs. found == FALSE means "no display", which is not an error. */
typedef struct boot_video_info {
  BOOLEAN found;
  UINT64  base_addr;
  UINT64  display;
  UINT64  row_bytes;
  UINT64  width;
  UINT64  height;
  UINT64  depth;
  UINT64  aperture_length;
  UINT32  pixel_format;
} boot_video_info;

EFI_STATUS boot_probe_video(AppContext *ctx, const CHAR8 *cmdline, boot_video_info *out);

EFI_STATUS boot_fill_video(AppContext *ctx, boot_args *args);
void boot_draw_logo(boot_args *args);
VOID boot_save_panic_log(AppContext *ctx);
BOOLEAN boot_cmdline_has_flag(const CHAR8 *cmdline, const CHAR8 *flag);
#if defined(__aarch64__) || defined(__riscv)
EFI_STATUS arm64_boot_fill_video(AppContext *ctx, arm64_boot_args *args);
#endif

VOID boot_free_args(AppContext *ctx, BootArgsState *state);

VOID boot_log_args(BootArgsState *state);

EFI_STATUS exit_boot_services_retry(AppContext *ctx, EFI_HANDLE image, BootArgsState *state);

#endif
