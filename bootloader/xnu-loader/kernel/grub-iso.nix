{ runCommand
, grub2
, xorriso
, mtools
, loaderKernel
, kernel
, bootArgs ? ""
}:

runCommand "xnu-loader-grub-iso" {
  nativeBuildInputs = [ grub2 xorriso mtools ];
} ''
  mkdir -p iso/boot/grub
  cp ${loaderKernel}/boot/xnu-loader.elf iso/boot/xnu-loader.elf
  cp ${kernel} iso/boot/kernel
  printf '%s' ${builtins.toJSON bootArgs} > iso/boot/boot-args.txt
  cat > iso/boot/grub/grub.cfg <<'EOF'
serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console
set timeout=1
menuentry "PureDarwin (xnu-loader multiboot2)" {
  multiboot2 /boot/xnu-loader.elf
  module2 /boot/kernel /EFI/BOOT/kernel
  module2 /boot/boot-args.txt /EFI/BOOT/boot-args.txt
  boot
}
EOF
  mkdir -p $out
  grub-mkrescue -o $out/xnu-loader-grub.iso iso
''
