#!/usr/bin/env python3
"""Build a current-kernel extension against the matching exported XNU SDK."""
import argparse
from pathlib import Path
import plistlib
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "hfs": ['core/BTree.c', 'core/BTreeAllocate.c', 'core/BTreeMiscOps.c', 'core/BTreeNodeOps.c', 'core/BTreeNodeReserve.c', 'core/BTreeScanner.c', 'core/BTreeTreeOps.c', 'core/BTreeWrapper.c', 'core/CatalogUtilities.c', 'core/FileExtentMapping.c', 'core/FileIDsServices.c', 'core/MacOSStubs.c', 'core/UnicodeWrappers.c', 'core/VolumeAllocation.c', 'core/hfs_attrlist.c', 'core/hfs_btreeio.c', 'core/hfs_catalog.c', 'core/hfs_chash.c', 'core/hfs_cnode.c', 'core/hfs_cprotect.c', 'core/hfs_endian.c', 'core/hfs_extents.c', 'core/hfs_fsinfo.c', 'core/hfs_hotfiles.c', 'core/hfs_journal.c', 'core/hfs_link.c', 'core/hfs_lookup.c', 'core/hfs_notification.c', 'core/hfs_quota.c', 'core/hfs_readwrite.c', 'core/hfs_resize.c', 'core/hfs_search.c', 'core/hfs_vfsops.c', 'core/hfs_vfsutils.c', 'core/hfs_vnops.c', 'core/hfs_xattr.c', 'core/rangelist.c', 'core/hfs_iokit.cpp'],
    "HFSEncodings": ["hfs_encodinghint.c", "hfs_encodings.c", "hfs_encodings_kext.cpp"],
    "embedded_crypto": ["embedded_crypto.c", "embedded_aes.c", "embedded_legacy_digest.c",
        "bearssl/src/hash/md5.c", "bearssl/src/hash/sha1.c",
        'bearssl/src/codec/dec32be.c', 'bearssl/src/codec/dec32le.c', 'bearssl/src/codec/enc32be.c', 'bearssl/src/codec/enc32le.c',
        "bearssl/src/symcipher/aes_ct.c", "bearssl/src/symcipher/aes_ct_enc.c",
        "bearssl/src/symcipher/aes_ct_dec.c", "bearssl/src/symcipher/aes_ct_cbcenc.c",
        "bearssl/src/symcipher/aes_ct_cbcdec.c"],
    "pthread": ["kern/kern_init.c", "kern/kern_support.c", "kern/kern_synch.c"],
    "AppleI386GenericPlatform": ["AppleI386CPU.cpp", "AppleI386PlatformExpert.cpp"],
    "AppleAPIC": ["Apple8259PIC.cpp", "AppleAPIC.cpp"],
    "IOStorageFamily": [
        "IOAppleLabelScheme.cpp", "IOApplePartitionScheme.cpp", "IOBlockStorageDevice.cpp",
        "IOBlockStorageDriver.cpp", "IOFDiskPartitionScheme.cpp", "IOFilterScheme.cpp",
        "IOGUIDPartitionScheme.cpp", "IOMedia.cpp", "IOMediaBSDClient.cpp",
        "IOPartitionScheme.cpp", "IOStorage.cpp",
    ],
    "BCM2711Platform": ["BCM2711Platform.cpp"],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--arch", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--component", choices=SOURCES, required=True)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--ld", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, work = args.root.resolve(), args.workdir.resolve()
    source = root / "src/Kernel/Extensions/25.5.0" / args.component
    if args.component == "HFSEncodings":
        source = source.parent / "hfs/hfs_encodings"
    if args.component == "pthread":
        source = root / "src/Libraries/25.5.0/libpthread"
    sdk = work / "sdk"
    sdk.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.sdk) as archive:
        archive.extractall(sdk, filter="data")
    bundle = work / (args.component + ".kext") / "Contents"
    (bundle / "MacOS").mkdir(parents=True, exist_ok=True)
    info_path = source / ("kern/pthread-Info.plist" if args.component == "pthread" else "Info.plist")
    if args.component == "HFSEncodings":
        info_path = source / "HFSEncodings-Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    if args.component == "pthread":
        info.update(CFBundleIdentifier="com.apple.kec.pthread", CFBundleName="pthread",
                    CFBundleVersion="1.0", AppleKernelExternalComponent=True)
    if args.component == "HFSEncodings":
        info.update(CFBundleIdentifier="com.apple.filesystems.hfs.encodings.kext",
                    CFBundleName="HFSEncodings", CFBundleVersion="1.0", CFBundleShortVersionString="1.0")
    info["CFBundleExecutable"] = args.component
    (bundle / "Info.plist").write_bytes(plistlib.dumps(info))
    flags = [f"--target={args.arch}-apple-macos26.5", "-mkernel", "-ffreestanding",
             "-fno-stack-protector", "-fno-builtin", "-fblocks", "-fno-common", "-O2",
             "-Wno-c99-designator", "-Wno-reorder-init-list", "-Wno-macro-redefined",
             "-DKERNEL=1", "-DKERNEL_PRIVATE=1", "-DPRIVATE=1", "-D__LITTLE_ENDIAN__",
             "-D__PUREDARWIN__=1", "-nostdinc", "-isystem",
             subprocess.check_output(["clang-20", "-print-resource-dir"], text=True).strip() + "/include",
             "-I", str(source / "include"), "-I", str(sdk / "usr/include"), "-I", str(source),
             "-include", str(root / "src/Kernel/xnu/12377.121.6/pd_apple_builtins.h")]
    if args.arch == "x86_64":
        flags += ["-mno-red-zone"]
    else:
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics", "-DARM64_BOARD_CONFIG_BCM2711"]
    if args.component == "embedded_crypto":
        flags += ["-I", str(source / "bearssl/inc"), "-I", str(source / "bearssl/src"),
                  "-I", str(sdk / "usr/include/libsa")]
    if args.component == "hfs":
        flags += ["-I", str(source / "core"), "-I", str(source / "hfs_encodings"),
                  "-include", str(source / "core/kext-config.h")]
    if args.component == "IOStorageFamily":
        flags += ["-include", str(sdk / "usr/include/libkern/OSMalloc.h")]
    if args.component == "pthread":
        flags += ["-DABSOLUTETIME_SCALAR_TYPE", "-DNEEDS_SCHED_CALL_T",
                  "-D__PTHREAD_EXPOSE_INTERNALS__", "-I", str(source / "private"),
                  "-I", str(sdk / "usr/include/libsa")]
    sources = [source / filename for filename in SOURCES[args.component]]

    if args.component != "embedded_crypto":
        metadata = work / "kmod_info.c"
        entry = ('extern kern_return_t pthread_start(kmod_info_t *, void *);\n'
                 'extern kern_return_t pthread_stop(kmod_info_t *, void *);\n') if args.component == "pthread" else (
                 'static kern_return_t module_start(kmod_info_t *m, void *p) '
                 '{ (void)m; (void)p; return KERN_SUCCESS; }\n'
                 'static kern_return_t module_stop(kmod_info_t *m, void *p) '
                 '{ (void)m; (void)p; return KERN_FAILURE; }\n')
        start, stop = ("pthread_start", "pthread_stop") if args.component == "pthread" else ("module_start", "module_stop")
        metadata.write_text('#include <mach/kmod.h>\n' + entry +
                            f'kmod_start_func_t *_realmain = {start};\n'
                            'extern kern_return_t _start(kmod_info_t *, void *);\n' +
                            f'KMOD_EXPLICIT_DECL({info["CFBundleIdentifier"]}, '
                            f'"{info["CFBundleVersion"]}", _start, {stop})\n')
        sources.append(metadata)
        sources.append(root / "src/Kernel/xnu/12377.121.6/libkern/kmod/c_start.c")
    objects = []
    for index, filename in enumerate(sources):
        output = work / f"source-{index}.o"
        cxx = filename.suffix == ".cpp"
        options = flags + (["-std=gnu++17", "-fapple-kext", "-fno-exceptions", "-fno-rtti",
                            "-fno-use-cxa-atexit", "-mllvm", "-disable-atexit-based-global-dtor-lowering"]
                           if cxx else ["-std=gnu17"])
        subprocess.run(["clang++-20" if cxx else "clang-20", *options,
                        "-c", str(filename), "-o", str(output)], check=True)
        objects.append(str(output))
    subprocess.run([str(args.ld.resolve()), "-arch", args.arch, "-kext",
                    "-undefined", "dynamic_lookup", "-o",
                    str(bundle / "MacOS" / args.component), *objects], check=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w") as archive:
        archive.add(bundle.parent, arcname=bundle.parent.name)


if __name__ == "__main__":
    main()
