{ runCommand
, grub2
, loaderKernel
}:

runCommand "xnu-loader-grub-bios" {
  nativeBuildInputs = [ grub2 ];
} ''
  mkdir -p $out
  grub-mkimage -O i386-pc -p '(hd0,gpt1)/boot/grub' -o $out/core.img \
    biosdisk part_gpt part_msdos fat multiboot2 linux serial terminal normal \
    configfile echo test video vbe
  cp ${grub2}/lib/grub/i386-pc/boot.img $out/boot.img
  cp ${loaderKernel}/boot/xnu-loader.elf $out/xnu-loader.elf
  cp ${loaderKernel}/boot/xnu-loader.bzImage $out/xnu-loader.bzImage
''
