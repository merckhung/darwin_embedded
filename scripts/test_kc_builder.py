"""Check native collection construction and rejection of missing imports."""
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
BUILDER = ROOT / "bazel-bin/tools/kc-tools/kc-builder"
KERNEL = ROOT / "bazel-bin/src/Kernel/xnu/12377.121.6/kernel.x86_64"
LINKER = ROOT / "bazel-bin/tools/cctools/ld"
ARM_KERNEL = KERNEL.with_name("kernel.arm64")


def commands(data, header=0):
    count = struct.unpack_from("<I", data, header + 16)[0]
    offset = header + 32
    for _ in range(count):
        kind, size = struct.unpack_from("<II", data, offset)
        yield kind, offset
        offset += size


def symbols(data, header=0):
    offset = next(offset for kind, offset in commands(data, header) if kind == 2)
    symoff, count, stroff, _ = struct.unpack_from("<4I", data, offset + 8)
    result = {}
    for index in range(count):
        name, kind, section, desc, value = struct.unpack_from("<IBBHQ", data, header + symoff + 16 * index)
        start = header + stroff + name
        result[data[start:data.index(0, start)].decode()] = value
    return result


def vm_bytes(data, header, address, size):
    for kind, offset in commands(data, header):
        if kind != 0x19:
            continue
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", data, offset + 24)
        if vmaddr <= address and address + size <= vmaddr + filesize:
            start = header + fileoff + address - vmaddr
            return data[start:start + size]
    raise AssertionError(f"unmapped address {address:#x}")


@unittest.skipUnless(BUILDER.is_file() and KERNEL.is_file(),
                     "build //:boot_img_current before running the collection tests")
class CollectionTests(unittest.TestCase):
    @unittest.skipUnless(ARM_KERNEL.is_file(), "requires the current ARM kernel build")
    def test_arm_child_data_extent_is_not_the_collection_aggregate(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "arm.kc"
            result = subprocess.run([str(BUILDER), "-kernel", str(ARM_KERNEL),
                                     "-kc-base", "fffffff006c00000", "-o", str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = output.read_bytes()
            entry = next(offset for kind, offset in commands(data) if kind == 0x80000035)
            child = struct.unpack_from("<Q", data, entry + 16)[0]
            def extent(contents, header):
                offset = next(offset for kind, offset in commands(contents, header)
                              if kind == 0x19 and contents[offset + 8:offset + 24].rstrip(b"\0") == b"__DATA_CONST")
                return struct.unpack_from("<2Q", contents, offset + 24)
            original = extent(ARM_KERNEL.read_bytes(), 0)
            self.assertEqual(extent(data, child), original)
            self.assertGreater(extent(data, 0)[1], original[1])

    @unittest.skipUnless(shutil.which("clang-20") and LINKER.is_file(), "requires Clang 20 and cctools ld")
    def test_classic_kext_pointer_and_branch_relocations(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            bundle = work / "Relocations.kext/Contents"
            (bundle / "MacOS").mkdir(parents=True)
            (bundle / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": "org.darwin-embedded.relocations-test",
                "CFBundleVersion": "1.0", "CFBundleExecutable": "Relocations"}))
            source = work / "relocations.c"
            source.write_text('extern int printf(const char *, ...);\n'
                              'int test_start(void) { return printf("relocations"); }\n'
                              'void *entry = (void *)test_start;\n'
                              'void *external = (void *)printf;\n')
            obj, binary = work / "relocations.o", bundle / "MacOS/Relocations"
            subprocess.run(["clang-20", "--target=x86_64-apple-macos26.5", "-mkernel",
                            "-fno-builtin", "-fno-stack-protector", "-c", str(source), "-o", str(obj)], check=True)
            subprocess.run([str(LINKER), "-arch", "x86_64", "-kext", "-undefined", "dynamic_lookup",
                            "-o", str(binary), str(obj)], check=True)
            original = binary.read_bytes()
            original_symbols = symbols(original)
            output = work / "kernel.kc"
            result = subprocess.run([str(BUILDER), "-kernel", str(KERNEL), "-kext", str(bundle.parent),
                                     "-o", str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            data = output.read_bytes()
            entries = [offset for kind, offset in commands(data) if kind == 0x80000035]
            header = struct.unpack_from("<Q", data, entries[-1] + 16)[0]
            linked_symbols = symbols(data, header)
            printf = symbols(KERNEL.read_bytes())["_printf"]
            self.assertEqual(struct.unpack("<Q", vm_bytes(data, header, linked_symbols["_entry"], 8))[0],
                             linked_symbols["_test_start"])
            self.assertEqual(struct.unpack("<Q", vm_bytes(data, header, linked_symbols["_external"], 8))[0], printf)
            # The compiler emits a direct call to printf. Verify its relocated target.
            code = vm_bytes(original, 0, original_symbols["_test_start"], 32)
            call = code.index(b"\xe8")
            displacement = struct.unpack("<i", vm_bytes(data, header, linked_symbols["_test_start"] + call + 1, 4))[0]
            self.assertEqual(linked_symbols["_test_start"] + call + 5 + displacement, printf)

    def test_kernel_only_collection_and_required_extensions(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = work / "kernel.kc"
            result = subprocess.run([str(BUILDER), "-kernel", str(KERNEL), "-o", str(output)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(struct.unpack_from("<I", output.read_bytes(), 12)[0], 12)
            output.unlink()
            result = subprocess.run([str(BUILDER), "-kernel", str(KERNEL),
                                     "-kext", str(work / "Missing.kext"), "-o", str(output)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertFalse(output.exists())

    @unittest.skipUnless(shutil.which("clang-20"), "requires Clang 20")
    def test_unresolved_relocation_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            bundle = work / "Unresolved.kext/Contents"
            (bundle / "MacOS").mkdir(parents=True)
            (bundle / "Info.plist").write_bytes(plistlib.dumps({
                "CFBundleIdentifier": "org.darwin-embedded.unresolved-test",
                "CFBundleVersion": "1.0", "CFBundleExecutable": "Unresolved"}))
            source = work / "missing.c"
            source.write_text("extern int unavailable_kernel_import(void);\n"
                              "int test_start(void) { return unavailable_kernel_import(); }\n")
            subprocess.run(["clang-20", "--target=x86_64-apple-macos26.5", "-ffreestanding",
                            "-fno-stack-protector", "-c", str(source),
                            "-o", str(bundle / "MacOS/Unresolved")], check=True)
            output = work / "invalid.kc"
            result = subprocess.run([str(BUILDER), "-kernel", str(KERNEL),
                                     "-kext", str(bundle.parent), "-o", str(output)],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertIn("unresolved", result.stderr.lower())
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
