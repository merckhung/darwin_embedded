#!/usr/bin/env bash
# Assemble BootKernelExtensions.kc (fileset KC) for the XNU/QEMU bring-up.
#
#  -kext     real, linked kexts (code loaded into the KC)
#  -codeless Info.plist-only interface/KPI kexts (dependency resolution only)
#
# The com.apple.kpi.* interface kexts live as System.kext PlugIns; they must be
# present as codeless bundles so OSBundleLibraries deps of the real kexts resolve.
set -euo pipefail

SCRIPT_PATH=${BASH_SOURCE[0]}
if command -v readlink >/dev/null 2>&1; then
  SCRIPT_PATH=$(readlink -f -- "$SCRIPT_PATH")
fi
SCRIPT_DIR=$(cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd)
DARWIN=$(cd -- "$SCRIPT_DIR/.." && pwd)
KEXTS=$DARWIN/PureDarwin/build-install/System/Library/Extensions
KERNEL=$DARWIN/PureDarwin/build-install/System/Library/Kernels/kernel.debug
OUT=${1:-$DARWIN/xnu-loader/esp/EFI/BOOT/kernel}
KCB=$DARWIN/kc-tools/result/bin/kc-builder

CODELESS=()
for p in "$KEXTS"/System.kext/PlugIns/*.kext; do
  CODELESS+=( -codeless "$p" )
done

REAL=(
  -kext "$KEXTS/corecrypto.kext"
  -kext "$KEXTS/pthread.kext"
  -kext "$KEXTS/IOACPIFamily.kext"
  -kext "$KEXTS/IOPCIFamily.kext"
  -kext "$KEXTS/AppleAPIC.kext"
  -kext "$KEXTS/AppleI386GenericPlatform.kext"
  -kext "$KEXTS/AppleI386PCI.kext"
  -kext "$KEXTS/IOStorageFamily.kext"
  -kext "$KEXTS/IOATAFamily.kext"
  -kext "$KEXTS/IOATAFamily.kext/Contents/PlugIns/AppleIntelPIIXATA.kext"
  -kext "$KEXTS/IOATABlockStorage.kext"
  -kext "$KEXTS/ext4.kext"
  -kext "$KEXTS/HFSEncodings.kext"
  -kext "$KEXTS/hfs.kext"
  -kext "$KEXTS/AppleFileSystemDriver.kext"
  -kext "$KEXTS/Ext4FileSystemDriver.kext"
  -kext "$KEXTS/IOHIDFamily.kext"      # legacy HID stack: IOHIKeyboard + IOBSDConsole
  -kext "$KEXTS/RavynAHCIPort.kext" # thanks Zoe
  -kext "$KEXTS/RavynXHCIPort.kext" # xHCI + USB mass storage (boot-from-USB)
  -kext "$KEXTS/IOGOPFramebuffer.kext" # thanks Zoe
  -kext "$KEXTS/ApplePS2Controller.kext" # i8042 + ApplePS2Keyboard (console input)
)

if [ "${MINIRV32:-0}" = "1" ]; then
  REAL+=( -kext "$KEXTS/MiniRV32IMA.kext" )
fi

CLASSIC_FLAG=()
if [ "${CLASSIC:-0}" = "1" ]; then
  # Skip the MH_FILESET/__KCHDR wrapper: i386_init.c's
  # kernel_mach_header_is_in_fileset() check then sees a plain prelinked
  # kernel and skips kernel_collection_slide() entirely, sidestepping its
  # address-computation bug for our synthetic __KCHDR layout (see
  # kc-tools/src/fileset.c's kchdr_vmaddr comment for the full story).
  CLASSIC_FLAG=(-classic)
fi

set -x
"$KCB" -kernel "$KERNEL" "${REAL[@]}" "${CODELESS[@]}" "${CLASSIC_FLAG[@]}" -o "$OUT"
