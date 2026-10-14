#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build the PC firmware application with Clang and the GNU-EFI runtime."""

import argparse
from pathlib import Path
import os
import struct
import subprocess
import tarfile
import tempfile


def check_application(path):
    """Require an x64 EFI application with a file-backed executable entry."""
    contents = path.read_bytes()
    if len(contents) < 64 or contents[:2] != b'MZ':
        raise ValueError('missing DOS/PE header')
    pe_offset = int.from_bytes(contents[60:64], 'little')
    if pe_offset > len(contents) - 24 or contents[pe_offset:pe_offset + 4] != b'PE\0\0':
        raise ValueError('invalid PE header location')
    machine, sections, _, _, _, optional_size, _ = struct.unpack_from('<HHIIIHH', contents, pe_offset + 4)
    optional = pe_offset + 24
    section_table = optional + optional_size
    if machine != 0x8664 or optional_size < 240 or sections < 1 or section_table + sections * 40 > len(contents):
        raise ValueError('invalid x64 COFF geometry')
    if struct.unpack_from('<H', contents, optional)[0] != 0x20b or struct.unpack_from('<H', contents, optional + 68)[0] != 10:
        raise ValueError('image is not a PE32+ EFI application')
    entry_rva = struct.unpack_from('<I', contents, optional + 16)[0]
    has_entry = False
    for index in range(sections):
        _, virtual_size, address, raw_size, raw_offset, _, _, _, _, flags = struct.unpack_from('<8sIIIIIIHHI', contents, section_table + index * 40)
        if raw_offset + raw_size > len(contents):
            raise ValueError('section exceeds output file')
        if flags & 0x20000000 and address <= entry_rva < address + min(virtual_size, raw_size):
            has_entry = True
    if not has_entry:
        raise ValueError('EFI entry is not inside file-backed executable code')


def build(options):
    project = options.project_root.resolve()
    sources = options.source.resolve()
    options.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=options.output.parent, prefix='efi-clang-') as temporary:
        scratch = Path(temporary).resolve()
        # Keep compiler scratch and extracted dependencies in this Bazel action.
        # No compiler process writes to a shared host temporary directory.
        environment = dict(os.environ, TMPDIR=str(scratch))
        dependency = scratch / 'dependency'
        with tarfile.open(options.runtime) as package:
            package.extractall(dependency, filter='data')
        library = dependency / 'usr/lib'
        headers = dependency / 'usr/include/efi'

        # One C translation unit gives the installer access to the parser and
        # loader helpers without maintaining a second object/dependency graph.
        # The assembly bridge stays separate so its ABI is visible for review.
        unit = scratch / 'application.c'
        unit.write_text('\n'.join('#include "' + str(sources / name) + '"' for name in ('image.c', 'loader.c')) + '\n')
        flags = ['--target=x86_64-unknown-linux-gnu', '-O2', '-fpic', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
                 '-fshort-wchar', '-mno-red-zone', '-mno-sse', '-mno-sse2',
                 '-DEFI_FUNCTION_WRAPPER', '-Wall', '-Wextra', '-Wno-pointer-sign',
                 '-I' + str(project), '-I' + str(headers), '-I' + str(headers / 'x86_64'),
                 '-I' + str(headers / 'protocol')]
        compiler = 'clang-20'
        for source, output in ((unit, 'application.o'), (sources / 'enter64.S', 'entry.o')):
            subprocess.run([compiler, *flags, '-c', str(source), '-o', output], cwd=scratch, env=environment, check=True)

        # GNU-EFI's ELF startup object relocates the application before calling
        # efi_main. Its linker script and runtime are the public EFI toolchain,
        # while the project supplies the application and native kernel ABI.
        builtins = subprocess.check_output([compiler, '--print-libgcc-file-name'], text=True).strip()
        subprocess.run(['ld', '-m', 'elf_x86_64', '-shared', '-Bsymbolic', '-nostdlib', '-znocombreloc',
                        '--no-undefined', '--no-warn-rwx-segments', '-T', str(library / 'elf_x86_64_efi.lds'),
                        str(library / 'crt0-efi-x86_64.o'), 'application.o', 'entry.o',
                        '-L' + str(library), '-lefi', '-lgnuefi', builtins, '-o', 'application.elf'],
                       cwd=scratch, env=environment, check=True)
        # Preserve the sections needed by the GNU-EFI relocation runtime.
        export = ['objcopy', '-O', 'efi-app-x86_64']
        for section in ('.text', '.data', '.sdata', '.rodata', '.dynamic', '.dynsym', '.rel', '.rela', '.reloc'):
            export.extend(['--only-section=' + section])
        subprocess.run([*export, 'application.elf', 'application.efi'], cwd=scratch, env=environment, check=True)
        image = scratch / 'application.efi'
        check_application(image)
        image.replace(options.output.resolve())
    print('PC UEFI: Clang application, native 64-bit entry bridge, validated PE32+ image', flush=True)


if __name__ == '__main__':
    arguments = argparse.ArgumentParser(description=__doc__)
    for name in ('project-root', 'source', 'runtime', 'output'):
        arguments.add_argument('--' + name, type=Path, required=True)
    build(arguments.parse_args())
