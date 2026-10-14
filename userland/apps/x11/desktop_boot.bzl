load("@rules_shell//shell:sh_binary.bzl", "sh_binary")

def xorg_boot(arch):
    kexts = (["embedded_crypto", "BCM2711Platform", "IOStorageFamily", "pthread", "HFSEncodings", "hfs"] if arch == "arm64" else
             ["embedded_crypto", "AppleI386GenericPlatform", "AppleAPIC", "IOStorageFamily", "pthread", "HFSEncodings", "hfs"])
    drivers = ["//userland/native/framebuffer:NativeBootFramebuffer." + arch + ".tar", "//userland/native/input:NativeBootInput." + arch + ".tar"]
    native.genrule(
        name = "desktop_collection_" + arch,
        srcs = drivers + ["//scripts:build_kernel_collection.py", "//src/Kernel/xnu/12377.121.6:kernel_" + arch,
                          "//src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar"] +
               ["//src/Kernel/Extensions/25.5.0:" + name + "_" + arch for name in kexts],
        tools = ["//tools/kc-tools:kc-builder"], outs = ["desktop-kernel.collection." + arch],
        cmd = "python3 $(location //scripts:build_kernel_collection.py) --builder $(location //tools/kc-tools:kc-builder)" +
              " --kernel $(location //src/Kernel/xnu/12377.121.6:kernel_" + arch + ")" +
              " --sdk $(location //src/Kernel/xnu/12377.121.6:kernel.headers." + arch + ".tar)" +
              "".join([" --kext $(location " + driver + ")" for driver in drivers]) +
              "".join([" --kext $(location //src/Kernel/Extensions/25.5.0:" + name + "_" + arch + ")" for name in kexts]) +
              (" --kc-base 0xfffffff006c00000" if arch == "arm64" else "") +
              " --workdir $(@D)/desktop-collection-" + arch + " --output $@",
    )
    if arch == "arm64":
        native.genrule(
            name = "desktop_kernel_arm64", srcs = [":desktop_collection_arm64", "//scripts:macho_to_bin.py"],
            outs = ["desktop-arm64/kernel.bin", "desktop-arm64/kernel_layout.h", "desktop-arm64/kernel_layout.json"],
            cmd = "python3 $(location //scripts:macho_to_bin.py) $(location :desktop_collection_arm64) $(location desktop-arm64/kernel.bin)" +
                  " --virtual-bias 0xfffffff000000000 --header $(location desktop-arm64/kernel_layout.h) --layout $(location desktop-arm64/kernel_layout.json)",
        )
        native.genrule(
            name = "desktop_loader_arm64",
            srcs = ["//bootloader/rpi4:boot_entry.S", "//bootloader/rpi4:boot_current.c", "//bootloader/rpi4:framebuffer.h",
                    "//bootloader/rpi4:boot.ld", "//bootloader/video:metadata.h", "//adt:current/adt_bin.h", ":desktop-arm64/kernel_layout.h"],
            outs = ["desktop-arm64/boot_current.bin"],
            cmd = "TMPDIR=$$PWD/$(@D) clang-20 --target=aarch64-none-elf -fuse-ld=lld -O2 -fno-pic -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -mgeneral-regs-only -mstrict-align" +
                  " -I$$(dirname $(location //adt:current/adt_bin.h)) -I$$(dirname $(location :desktop-arm64/kernel_layout.h))" +
                  " -Wl,-T$(location //bootloader/rpi4:boot.ld) -Wl,--build-id=none -o $(@D)/boot_current.elf" +
                  " $(location //bootloader/rpi4:boot_entry.S) $(location //bootloader/rpi4:boot_current.c) &&" +
                  " llvm-objcopy-20 -S -O binary $(@D)/boot_current.elf $@",
        )
        native.filegroup(name = "xorg_boot_arm64", srcs = [":desktop_kernel_arm64", ":desktop_loader_arm64", ":xorg_root_arm64"])
    else:
        native.genrule(
            name = "xorg_boot_x86_64",
            srcs = [":desktop_collection_x86_64", ":xorg-root.x86_64.img", "//bootloader/pc_uefi:efi", "//scripts:create_efi_image.py"],
            outs = ["xorg-boot.x86_64.img"],
            cmd = "python3 $(location //scripts:create_efi_image.py) --loader $(location //bootloader/pc_uefi:efi)" +
                  " --kernel-collection $(location :desktop_collection_x86_64) --root-image $(location :xorg-root.x86_64.img) --output $@",
        )
    sh_binary(
        name = "verify_xorg_" + arch, srcs = ["verify_xorg.sh"], args = [arch],
        data = [":xorg_boot_" + arch, "verify_xorg.py", "//userland/native/framebuffer:verify.py", "//bootloader/ovmf:firmware"],
    )
