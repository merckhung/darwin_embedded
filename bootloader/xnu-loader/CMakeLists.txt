cmake_minimum_required(VERSION 3.16)
project(xnu_loader C)

enable_language(ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED GNU_EFI_DIR)
  message(FATAL_ERROR "GNU_EFI_DIR must be set")
endif()

if(NOT ARCH)
  set(ARCH x86_64)
endif()
set(ARCH "${ARCH}" CACHE STRING "Kernel/payload architecture (x86_64 or aarch64)")

if(NOT ARCH STREQUAL "x86_64" AND NOT ARCH STREQUAL "aarch64")
  message(FATAL_ERROR "ARCH must be x86_64 or aarch64, got '${ARCH}'")
endif()

# The firmware's own architecture, which is not always the kernel's: a 32-bit
# UEFI implementation on a 64-bit CPU only loads IA32 PE images, so the loader
# is built for ia32 while the kernel it hands off to stays x86_64. Defaults to
# ARCH, which is every case where the two agree.
if(NOT LOADER_ARCH)
  set(LOADER_ARCH ${ARCH})
endif()
set(LOADER_ARCH "${LOADER_ARCH}" CACHE STRING "Firmware architecture (x86_64, aarch64 or ia32)")

if(NOT LOADER_ARCH STREQUAL "x86_64" AND NOT LOADER_ARCH STREQUAL "aarch64"
   AND NOT LOADER_ARCH STREQUAL "ia32")
  message(FATAL_ERROR "LOADER_ARCH must be x86_64, aarch64 or ia32, got '${LOADER_ARCH}'")
endif()

if(LOADER_ARCH STREQUAL "ia32" AND NOT ARCH STREQUAL "x86_64")
  message(FATAL_ERROR "LOADER_ARCH=ia32 only boots an x86_64 kernel, not '${ARCH}'")
endif()

message(STATUS "loader arch: ${LOADER_ARCH}, kernel arch: ${ARCH}")

set(GNU_EFI_INC
  ${GNU_EFI_DIR}/include
  ${GNU_EFI_DIR}/include/efi
  ${GNU_EFI_DIR}/include/efi/${LOADER_ARCH}
  ${GNU_EFI_DIR}/include/protocol
  ${GNU_EFI_DIR}/include/efi/protocol
)

set(GNU_EFI_LIB_DIR ${GNU_EFI_DIR}/lib)
set(GNU_EFI_LDS ${GNU_EFI_LIB_DIR}/elf_${LOADER_ARCH}_efi.lds)
set(GNU_EFI_CRT ${GNU_EFI_LIB_DIR}/crt0-efi-${LOADER_ARCH}.o)

set(EFI_LINK_EMULATION "")

if(LOADER_ARCH STREQUAL "x86_64")
  set(ARCH_M_FLAG -m64)
  set(EFI_INPUT_TARGET elf64-x86-64)
elseif(LOADER_ARCH STREQUAL "ia32")
  set(ARCH_M_FLAG -m32)
  set(EFI_INPUT_TARGET elf32-i386)
  # ld defaults to the 64-bit emulation when the toolchain is x86_64.
  set(EFI_LINK_EMULATION -m elf_i386)
else()
  set(ARCH_M_FLAG "")
  set(EFI_INPUT_TARGET elf64-littleaarch64)
endif()

execute_process(
  COMMAND ${CMAKE_C_COMPILER} ${ARCH_M_FLAG} -print-libgcc-file-name
  OUTPUT_VARIABLE LIBGCC_FILE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "libgcc: ${LIBGCC_FILE}")

set(SOURCES
  src/main.c
  src/app.c
  src/boot.c
  src/logo.c
  src/panic.c
  src/console.c
  src/devtree.c
  src/fdt_board.c
  src/devtree-fdt.c
  src/fileio.c
  src/jump.S
  src/lowmem.c
  src/macho.c
  src/serial.c
)

add_library(loader_objs OBJECT ${SOURCES})

target_include_directories(loader_objs PRIVATE
  ${GNU_EFI_INC}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(loader_objs PRIVATE
  -ffreestanding
  -fno-stack-protector
  -fno-stack-check
  -funsigned-char
  -fshort-wchar
  -fpie
  -Wall
  -Wextra
  -Wno-pointer-sign
  -ggdb
)

if(LOADER_ARCH STREQUAL "x86_64" OR LOADER_ARCH STREQUAL "ia32")
  # Pack outgoing-arg stack slots instead of sub/add-per-call (gnu-efi's
  # calling convention expectation), and no AVX (SSE/AVX state isn't
  # guaranteed usable this early in boot).
  target_compile_options(loader_objs PRIVATE
    ${ARCH_M_FLAG}
    -maccumulate-outgoing-args
    -mno-avx
  )
endif()

if(LOADER_ARCH STREQUAL "x86_64")
  # No red zone: interrupts/exceptions can clobber it before a real handler
  # runs. 32-bit has no red zone to disable.
  target_compile_options(loader_objs PRIVATE -mno-red-zone)
endif()

# Which aarch64 machine to target. The platforms differ in DRAM base, UART
# hardware and which devicetree nodes XNU expects, so exactly one is selected
# at build time and surfaced to the sources as XNU_LOADER_PLATFORM_<NAME>.
# Ignored for x86_64, which has no equivalent variation.
set(XNU_LOADER_PLATFORM "bcm2837" CACHE STRING
    "aarch64 target platform: bcm2837, generic, qemuvirt, sun50i or sg2002")
set_property(CACHE XNU_LOADER_PLATFORM PROPERTY STRINGS bcm2837 generic qemuvirt sun50i sg2002)

set(XNU_LOADER_PLATFORM_DEFINES "")
if(ARCH STREQUAL "aarch64")
  if(XNU_LOADER_PLATFORM STREQUAL "bcm2837")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_BCM2837)
  elseif(XNU_LOADER_PLATFORM STREQUAL "generic")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_GENERIC)
  elseif(XNU_LOADER_PLATFORM STREQUAL "qemuvirt")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_QEMUVIRT)
  elseif(XNU_LOADER_PLATFORM STREQUAL "sun50i")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_SUN50I)
  elseif(XNU_LOADER_PLATFORM STREQUAL "sg2002")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_SG2002)
  elseif(XNU_LOADER_PLATFORM STREQUAL "sc8280xp")
    set(XNU_LOADER_PLATFORM_DEFINES XNU_LOADER_PLATFORM_SC8280XP)
  else()
    message(FATAL_ERROR
      "XNU_LOADER_PLATFORM must be bcm2837, generic, qemuvirt, sun50i, sg2002 or sc8280xp, "
      "got '${XNU_LOADER_PLATFORM}'")
  endif()
  message(STATUS "aarch64 platform: ${XNU_LOADER_PLATFORM}")
endif()

target_compile_definitions(loader_objs PRIVATE
  EFI_FUNCTION_WRAPPER
  CONFIG_${ARCH}
  CONFIG_LOADER_${LOADER_ARCH}
  ${XNU_LOADER_PLATFORM_DEFINES}
)

add_custom_command(
  OUTPUT xnu-loader.so
  COMMAND_EXPAND_LISTS
  COMMAND ${CMAKE_LINKER}
          -nostdlib
          --warn-common
          --no-undefined
          --fatal-warnings
          --no-warn-rwx-segments
          -znocombreloc
          ${EFI_LINK_EMULATION}
          -shared
          -Bsymbolic
          -L${GNU_EFI_LIB_DIR}
          ${GNU_EFI_CRT}
          $<TARGET_OBJECTS:loader_objs>
          -o xnu-loader.so
          -lgnuefi
          -lefi
          -T ${GNU_EFI_LDS}
          $<$<BOOL:${LIBGCC_FILE}>:${LIBGCC_FILE}>
  DEPENDS loader_objs
  VERBATIM
)

add_custom_command(
  OUTPUT xnu-loader.efi
  COMMAND ${CMAKE_OBJCOPY}
          --input-target=${EFI_INPUT_TARGET}
          -j .text
          -j .sdata
          -j .data
          -j .rodata
          -j .dynamic
          -j .dynsym
          -j .rel
          -j .rela
          -j .reloc
          --output-target=efi-app-${LOADER_ARCH}
          xnu-loader.so xnu-loader.efi
  DEPENDS xnu-loader.so
  VERBATIM
)

add_custom_target(xnu-loader ALL DEPENDS xnu-loader.efi)
