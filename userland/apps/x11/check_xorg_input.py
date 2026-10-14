#!/usr/bin/env python3
"""Audit native Xorg input providers and compile its actual XKB keycode mapping."""
import json
import ctypes
import ctypes.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from check_xorg_display import sha, validate_object, symbols, tar_files, image_metadata


class NativeInputArtifacts(unittest.TestCase):
    def test_target_objects_and_actual_source_providers(self):
        for arch, cpu in (("arm64", 0x100000c), ("x86_64", 0x1000007)):
            path = Path(f"userland/apps/x11/xorg-input.{arch}.o")
            data = path.read_bytes()
            record = json.loads(path.with_suffix(".json").read_text())
            self.assertEqual(sha(data), record["object_sha256"])
            self.assertEqual(sha(Path("userland/apps/x11/xorg_input.c").read_bytes()), record["source_sha256"])
            self.assertEqual(sha(Path("userland/apps/x11/data/keycodes/nativehid").read_bytes()), record["native_keycodes_sha256"])
            self.assertEqual(sha(Path("userland/apps/x11/data/rules/nativehid").read_bytes()), record["native_rules_sha256"])
            validate_object(data, cpu)
            with self.assertRaises(ValueError): validate_object(data, 0x1000007 if cpu == 0x100000c else 0x100000c)
            with self.assertRaises(ValueError): validate_object(b"\x7fELF" + data[4:], cpu)
            actual = symbols(path)
            self.assertEqual(actual[0], set(record["exports"]))
            self.assertEqual(actual[1], set(record["imports"]))
            self.assertTrue({"_InitInput", "_CloseInput", "_ProcessInputEvents", "_SetDeviceMode", "_DeleteInputDeviceRequest"} <= actual[0])
            self.assertTrue({"_IOConnectCallStructMethod", "_IOServiceOpen", "_IOServiceClose", "_InitKeyboardDeviceStruct", "_XkbSetRulesDflts",
                             "_QueueKeyboardEvents", "_QueuePointerEvents", "_TimerSet", "_TimerFree", "_mieqInit"} <= actual[1])
            core = json.loads(Path(f"userland/apps/x11/xorg-core.{arch}.json").read_text())
            available = set(core["definitions"])
            for library in ("iokit/libnative_iokit", "libc/libc_base"):
                available |= symbols(Path(f"userland/native/{library}.{arch}.a"))[0]
            for name, contents in tar_files(Path(f"userland/native/framebuffer/system_{arch}.tar")).items():
                if name.endswith(".dylib"): available |= image_metadata(contents)["exports"]
            self.assertEqual(actual[1] - available, set())
            self.assertTrue(record["input_backend_supplied"])
            self.assertFalse(record["runtime_checked"])
            self.assertFalse(record["server_program_linked"])

    def test_keymap_compiler_symbols_and_modifiers(self):
        # Host compiler validates portable data only; guest execution is pending.
        directory = Path("userland/apps/x11/data").resolve()
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR")) as temporary:
            source = Path(temporary) / "native.xkb"
            output = Path(temporary) / "compiled.xkb"
            source.write_text('xkb_keymap { xkb_keycodes { include "nativehid" }; '
                              'xkb_types { include "complete" }; xkb_compatibility { include "complete" }; '
                              'xkb_symbols { include "pc+us" }; };\n')
            result = subprocess.run(["xkbcomp", "-I" + str(directory), "-xkb", str(source), str(output)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = output.read_text()
            for key, code in (("AC01",12), ("LEFT",88), ("LFSH",233), ("RCTL",236), ("RTRN",48)):
                self.assertRegex(text, r"<" + key + r">\s*=\s*" + str(code) + r";")
            for key, pattern in (("AC01", r"\[\s*a,\s*A\s*\]"), ("LEFT", r"\[\s*Left\s*\]")):
                block = re.search(r"key\s+<" + key + r">\s*\{([^}]+)\}", text)
                self.assertIsNotNone(block, key)
                self.assertRegex(block[1], pattern)
            self.assertRegex(text, r"modifier_map Shift\s*\{[^}]*<LFSH>")
            self.assertRegex(text, r"modifier_map Control\s*\{[^}]*<RCTL>")
            definitions = re.findall(r"<(\w+)>\s*=\s*(\d+);", text)
            codes = [int(code) for _, code in definitions]
            self.assertEqual(len(codes), len(set(codes)))

    def test_native_rules_resolve_real_layouts_and_variants(self):
        class Variables(ctypes.Structure):
            _fields_ = [(name, ctypes.c_char_p) for name in ("model", "layout", "variant", "options")] + [
                ("sz_extra", ctypes.c_ushort), ("num_extra", ctypes.c_ushort), ("extra_names", ctypes.c_char_p),
                ("extra_values", ctypes.POINTER(ctypes.c_char_p))]
        class Components(ctypes.Structure):
            _fields_ = [(name, ctypes.c_void_p) for name in ("keymap", "keycodes", "types", "compat", "symbols", "geometry")]
        # Host public XKB rule parser checks data; native guest use is pending.
        library = ctypes.CDLL(ctypes.util.find_library("xkbfile"))
        library.XkbRF_Load.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        library.XkbRF_Load.restype = ctypes.c_void_p
        library.XkbRF_GetComponents.argtypes = [ctypes.c_void_p, ctypes.POINTER(Variables), ctypes.POINTER(Components)]
        library.XkbRF_GetComponents.restype = ctypes.c_int
        library.XkbRF_Free.argtypes = [ctypes.c_void_p, ctypes.c_int]
        allocator = ctypes.CDLL(None)
        allocator.free.argtypes = [ctypes.c_void_p]
        rules = library.XkbRF_Load(str(Path("userland/apps/x11/data/rules/nativehid").resolve()).encode(), None, 0, 1)
        self.assertTrue(rules)
        try:
            for layout, variant, symbols in ((b"us", b"", b"pc+us"), (b"gb", b"", b"pc+gb"), (b"us", b"intl", b"pc+us(intl)")):
                variables = Variables(model=b"pc105", layout=layout, variant=variant, options=b"")
                names = Components()
                try:
                    self.assertTrue(library.XkbRF_GetComponents(rules, ctypes.byref(variables), ctypes.byref(names)))
                    for field, expected in (("keycodes",b"nativehid"), ("types",b"complete"), ("compat",b"complete"),
                                            ("symbols",symbols), ("geometry",b"pc(pc105)")):
                        self.assertEqual(ctypes.string_at(getattr(names,field)), expected)
                finally:
                    for field, _ in names._fields_:
                        if getattr(names,field): allocator.free(getattr(names,field))
        finally:
            library.XkbRF_Free(rules, 1)


if __name__ == "__main__": unittest.main()
