load("@rules_shell//shell:sh_binary.bzl", "sh_binary")
load("//userland/native/system:defs.bzl", "native_system")

def input_port(arch):
    native.genrule(
        name = "driver_" + arch,
        srcs = ["NativeBootInput.cpp", "NativeBootInput.h", "ps2_decode.h", "Info.plist",
                "//userland/native/framebuffer:build_driver.py", "//src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar",
                "//src/Kernel/xnu/12377.121.6:pd_apple_builtins.h", "//src/Kernel/xnu/12377.121.6:libkern/kmod/c_start.c"] +
               (["NativeDWC2.cpp", "NativeDWC2.h", "hid_decode.h"] if arch == "arm64" else []),
        tools = ["//tools/cctools:ld"], outs = ["NativeBootInput." + arch + ".tar", "driver." + arch + ".json"],
        cmd = "python3 $(location //userland/native/framebuffer:build_driver.py) --root $$PWD --arch " + arch + " --component input" +
              " --sdk $(location //src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar) --ld $(location //tools/cctools:ld)" +
              " --output $(location NativeBootInput." + arch + ".tar) --report $(location driver." + arch + ".json)",
    )
    kexts = (["embedded_crypto", "BCM2711Platform", "IOStorageFamily", "pthread", "HFSEncodings", "hfs"] if arch == "arm64" else
             ["embedded_crypto", "AppleI386GenericPlatform", "AppleAPIC", "IOStorageFamily", "pthread", "HFSEncodings", "hfs"])
    native.genrule(
        name = "collection_" + arch,
        srcs = [":NativeBootInput." + arch + ".tar", "//scripts:build_kernel_collection.py",
                "//src/Kernel/xnu/12377.121.6:kernel_" + arch, "//src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar"] +
               ["//src/Kernel/Extensions/25.5.0:" + name + "_" + arch for name in kexts],
        tools = ["//tools/kc-tools:kc-builder"], outs = ["kernel.collection." + arch],
        cmd = "python3 $(location //scripts:build_kernel_collection.py) --builder $(location //tools/kc-tools:kc-builder)" +
              " --kernel $(location //src/Kernel/xnu/12377.121.6:kernel_" + arch + ")" +
              " --sdk $(location //src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar)" +
              " --kext $(location :NativeBootInput." + arch + ".tar)" +
              "".join([" --kext $(location //src/Kernel/Extensions/25.5.0:" + name + "_" + arch + ")" for name in kexts]) +
              (" --kc-base 0xfffffff006c00000" if arch == "arm64" else "") +
              " --workdir $(@D)/collection-" + arch + " --output $@",
    )
    native.genrule(
        name = "probe_" + arch,
        srcs = ["probe.c", "NativeBootInput.h", "//userland/native/framebuffer:build_probe.py",
                "//scripts:native_components.py", "//userland/native:sdk_" + arch, "//userland/native:mach.headers." + arch + ".tar",
                "//userland/native/iokit:iokit.headers." + arch + ".tar", "//userland/apps/init:system." + arch + ".exports"],
        outs = ["probe." + arch + ".o", "probe." + arch + ".exports"],
        cmd = "python3 $(location //userland/native/framebuffer:build_probe.py) --arch " + arch + " --source $(location probe.c)" +
              " --sdk $(location //userland/native:sdk_" + arch + ") --mach-headers $(location //userland/native:mach.headers." + arch + ".tar)" +
              " --iokit-headers $(location //userland/native/iokit:iokit.headers." + arch + ".tar)" +
              " --base-exports $(location //userland/apps/init:system." + arch + ".exports)" +
              " --output $(location probe." + arch + ".o) --exports $(location probe." + arch + ".exports)",
    )
    native_system(
        name = "system_" + arch, arch = arch, exports = ":probe." + arch + ".exports",
        libraries = {"accounts": "//userland/native/accounts:libaccounts." + arch + ".a",
                     "resolver": "//userland/native/resolver:libresolver." + arch + ".a",
                     "permissions": "//userland/apps/glib:libglib_support." + arch + ".a",
                     "init_support": "//userland/apps/init:libinit_support." + arch + ".a",
                     "iokit": "//userland/native/iokit:libnative_iokit." + arch + ".a"},
    )
    native.genrule(
        name = "root_" + arch,
        srcs = ["//userland/native/framebuffer:build_root.py", ":probe." + arch + ".o", ":system_" + arch + ".tar",
                "//userland/apps/init:root." + arch + ".img", "//userland/apps/init:root." + arch + ".json", "//userland/native:dyld_" + arch, "//scripts:native_components.py",
                "//scripts:build_diagnostic_userland.py", "//scripts:sign_diagnostic_macho.py"],
        tools = ["//tools/libdmg-hfsplus:hfsplus"], outs = ["root." + arch + ".img", "root." + arch + ".json"],
        cmd = "python3 $(location //userland/native/framebuffer:build_root.py) --arch " + arch + " --program native-input-test" +
              " --probe $(location probe." + arch + ".o) --system $(location system_" + arch + ".tar)" +
              " --base-root $(location //userland/apps/init:root." + arch + ".img) --base-report $(location //userland/apps/init:root." + arch + ".json) --dyld $(location //userland/native:dyld_" + arch + ")" +
              " --hfs-tool $(location //tools/libdmg-hfsplus:hfsplus) --output $(location root." + arch + ".img) --report $(location root." + arch + ".json)",
    )
    if arch == "arm64":
        native.genrule(
            name = "kernel_arm64", srcs = [":collection_arm64", "//scripts:macho_to_bin.py"],
            outs = ["arm64/kernel.bin", "arm64/kernel_layout.h", "arm64/kernel_layout.json"],
            cmd = "python3 $(location //scripts:macho_to_bin.py) $(location :collection_arm64) $(location arm64/kernel.bin)" +
                  " --virtual-bias 0xfffffff000000000 --header $(location arm64/kernel_layout.h) --layout $(location arm64/kernel_layout.json)",
        )
        native.genrule(
            name = "loader_arm64",
            srcs = ["//bootloader/rpi4:boot_entry.S", "//bootloader/rpi4:boot_current.c", "//bootloader/rpi4:framebuffer.h",
                    "//bootloader/rpi4:boot.ld", "//bootloader/video:metadata.h", "//adt:current/adt_bin.h", ":arm64/kernel_layout.h"],
            outs = ["arm64/boot_current.bin"],
            cmd = "TMPDIR=$$PWD/$(@D) clang-20 --target=aarch64-none-elf -fuse-ld=lld -O2 -fno-pic -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -mgeneral-regs-only -mstrict-align" +
                  " -I$$(dirname $(location //adt:current/adt_bin.h)) -I$$(dirname $(location :arm64/kernel_layout.h))" +
                  " -Wl,-T$(location //bootloader/rpi4:boot.ld) -Wl,--build-id=none -o $(@D)/boot_current.elf" +
                  " $(location //bootloader/rpi4:boot_entry.S) $(location //bootloader/rpi4:boot_current.c) &&" +
                  " llvm-objcopy-20 -S -O binary $(@D)/boot_current.elf $@",
        )
        native.filegroup(name = "boot_arm64", srcs = [":kernel_arm64", ":loader_arm64", ":root_arm64"])
    else:
        native.genrule(
            name = "boot_x86_64",
            srcs = [":collection_x86_64", ":root.x86_64.img", "//bootloader/pc_uefi:efi", "//scripts:create_efi_image.py"],
            outs = ["boot.x86_64.img"],
            cmd = "python3 $(location //scripts:create_efi_image.py) --loader $(location //bootloader/pc_uefi:efi)" +
                  " --kernel-collection $(location :collection_x86_64) --root-image $(location :root.x86_64.img) --output $@",
        )
    sh_binary(
        name = "verify_" + arch, srcs = ["verify.sh"], args = ["--arch", arch],
        data = [":boot_" + arch, "verify.py", "//userland/native/framebuffer:verify.py", "//bootloader/ovmf:firmware"],
    )
