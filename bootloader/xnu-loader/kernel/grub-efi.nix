{ runCommand
, grub2_efi
, loaderKernel
}:

runCommand "xnu-loader-grub-efi" {
  nativeBuildInputs = [ grub2_efi ];
} ''
  mkdir -p $out
  grub-mkimage -O x86_64-efi -p '(hd0,gpt1)/boot/grub' -o $out/grubx64.efi \
    part_gpt part_msdos fat multiboot2 linux serial terminal normal configfile \
    echo test efi_gop efi_uga all_video video
  cp ${loaderKernel}/boot/xnu-loader.elf $out/xnu-loader.elf
  cp ${loaderKernel}/boot/xnu-loader.bzImage $out/xnu-loader.bzImage
''
