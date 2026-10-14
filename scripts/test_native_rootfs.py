"""Exercise rejection of foreign programs and unexpected files in minimal roots."""
from pathlib import Path
import struct
import tempfile
import unittest

from build_diagnostic_userland import CPU_TYPES, PROGRAM_TYPES, verify_installed_files


class NativeRootTests(unittest.TestCase):
    def make_root(self, directory, arch):
        expected = {name: struct.pack("<8I", 0xfeedfacf, CPU_TYPES[arch], 0, kind, 0, 0, 0, 0)
                    for name, kind in PROGRAM_TYPES.items()}
        expected["/etc/os-release"] = b'NAME="Darwin Embedded"\n'
        for name, data in expected.items():
            path = directory / name.lstrip("/")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return expected

    def test_both_native_architectures(self):
        with tempfile.TemporaryDirectory() as temporary:
            for arch in CPU_TYPES:
                with self.subTest(arch=arch):
                    root = Path(temporary) / arch
                    verify_installed_files(root, arch, self.make_root(root, arch))

    def test_reject_elf_program_or_data_even_if_it_matches_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = self.make_root(root, "arm64")
            for name in ("/bin/sh", "/etc/os-release"):
                with self.subTest(name=name):
                    saved = expected[name]
                    expected[name] = b"\x7fELF" + b"\0" * 28
                    (root / name.lstrip("/")).write_bytes(expected[name])
                    with self.assertRaisesRegex(ValueError, "ELF is forbidden"):
                        verify_installed_files(root, "arm64", expected)
                    expected[name] = saved
                    (root / name.lstrip("/")).write_bytes(saved)

    def test_reject_wrong_architecture_or_file_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = self.make_root(root, "arm64")
            for cpu, kind in ((CPU_TYPES["x86_64"], 2), (CPU_TYPES["arm64"], 6)):
                with self.subTest(cpu=cpu, kind=kind):
                    expected["/bin/sh"] = struct.pack("<8I", 0xfeedfacf, cpu, 0, kind, 0, 0, 0, 0)
                    (root / "bin/sh").write_bytes(expected["/bin/sh"])
                    with self.assertRaisesRegex(ValueError, "expected native arm64 Mach-O"):
                        verify_installed_files(root, "arm64", expected)

    def test_reject_added_missing_or_changed_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = self.make_root(root, "x86_64")
            extra = root / "bin/foreign"
            extra.write_bytes(b"\x7fELF")
            with self.assertRaisesRegex(ValueError, "unexpected root contents"):
                verify_installed_files(root, "x86_64", expected)
            extra.unlink()
            release = root / "etc/os-release"
            release.unlink()
            with self.assertRaisesRegex(ValueError, "unexpected root contents"):
                verify_installed_files(root, "x86_64", expected)
            release.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "does not match its source"):
                verify_installed_files(root, "x86_64", expected)


if __name__ == "__main__":
    unittest.main()
