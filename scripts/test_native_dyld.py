"""Exercise the actual native Mach-O parser/fixups in a host-only harness.

The Mach-O definitions are Apple's imported headers. Only their platform type
includes are replaced for host compilation; loader.c is included unchanged.
Host mocks capture diagnostics and protection requests, and abort if mapping or
other native calls unexpectedly occur. QEMU checks cover those real operations.
This harness and its host executable are never installed in a guest image.
"""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from build_native_dyld import generate_metadata_headers

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / "src/Kernel/xnu/12377.121.6/EXTERNAL_HEADERS/mach-o"

HARNESS = r'''
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "loader.c"
static jmp_buf rejection;
static char diagnostic[2048];
static size_t diagnostic_size;
static int protection;
static uintptr_t section_address;
static unsigned long section_size;
static unsigned security_calls, credential_calls;
static int security_status, changed_credentials;
static uint32_t observed_security_flags;
const struct mach_header_64 _mh_dylinker_header = {0};
long __write_nocancel(int fd, const void *data, size_t size) {
    (void)fd;
    size_t remaining = sizeof(diagnostic) - diagnostic_size - 1;
    size_t copied = size < remaining ? size : remaining;
    memcpy(diagnostic + diagnostic_size, data, copied);
    diagnostic_size += copied;
    diagnostic[diagnostic_size] = 0;
    return (long)size;
}
_Noreturn void __exit(int status) { longjmp(rejection, status); }
int __open_nocancel(const char *path, int flags, int mode) { abort(); }
int __close_nocancel(int fd) { abort(); }
long __pread_nocancel(int fd, void *data, size_t size, int64_t offset) { abort(); }
int __fcntl_nocancel(int fd, int command, void *value) { abort(); }
void *__mmap(void *address, size_t size, int prot, int flags, int fd, int64_t offset) { abort(); }
int __munmap(void *address, size_t size) { abort(); }
uint32_t thread_self_trap(void) { abort(); }
int swtch(void) { abort(); }
int __mprotect(void *address, size_t size, int prot) {
    (void)address; (void)size; protection = prot; return 0;
}
void _thread_set_tsd_base(void *base) { abort(); }
int csops(int pid, unsigned command, void *value, size_t size) {
    if (pid || command != NATIVE_CS_OPS_STATUS || size != sizeof(uint32_t)) abort();
    ++security_calls;
    if (!security_status) *(uint32_t *)value = observed_security_flags;
    return security_status;
}
int issetugid(void) { ++credential_calls; return changed_credentials; }
int inspect(void *data, unsigned kind, unsigned operation) {
    diagnostic_size = 0; diagnostic[0] = 0; protection = -1;
    memset(images, 0, sizeof(images)); image_count = 1;
    images[0].permanent = images[0].global = 1;
    loader_lock_owner = 0; loader_lock_depth = 0;
    memset(&linker_image, 0, sizeof(linker_image));
    memset(&security_policy, 0, sizeof(security_policy));
    security_calls = credential_calls = 0;
    copy_path(images[0].path, "/test/native-image");
    int status = setjmp(rejection);
    if (status) return status;
    parse_image(images, data, kind);
    if (operation == 1) rebase(images);
    if (operation == 2) bind(images, images->info->bind_off, images->info->bind_size, 0, 0);
    if (operation == 3) writable_fixups(images, 1);
    if (operation == 4) section_address = (uintptr_t)getsectiondata(data, "__TEXT", "__text", &section_size);
    if (operation == 5) {
        struct native_dl_info result;
        dladdr((const char *)data + 2155, &result);
    }
    return 0;
}
const char *last_error(void) { return diagnostic; }
int last_protection(void) { return protection; }
uintptr_t last_section(void) { return section_address; }
unsigned long last_section_size(void) { return section_size; }
int inspect_export(const char *name, uintptr_t *address) { return find_export(images, name, 0, address); }
int inspect_transaction(void) {
    static struct load_transaction transaction;
    memset(&transaction, 0, sizeof(transaction));
    lock_loader();
    active_transaction = &transaction;
    if (!_setjmp(transaction.recovery)) fail("test transactional rejection", "native import");
    active_transaction = 0;
    int valid = same(transaction.error, "test transactional rejection: native import");
    unlock_loader();
    return valid;
}
void set_security(int status, uint32_t flags, int changed) {
    security_status = status; observed_security_flags = flags; changed_credentials = changed;
    memset(&security_policy, 0, sizeof(security_policy));
    security_calls = credential_calls = 0;
}
unsigned security_observation(unsigned field) {
    switch (field) {
    case 0: return security_calls;
    case 1: return credential_calls;
    case 2: return security_policy.code_signing_known;
    case 3: return security_policy.code_signing_flags;
    case 4: return security_policy.changed_credentials;
    case 5: return security_policy.restrict_section;
    default: abort();
    }
}
unsigned version_map_count(void) { return sizeof(native_version_sets) / sizeof(native_version_sets[0]); }
uint32_t version_map_field(unsigned row, unsigned field) {
    if (row >= version_map_count() || field > 1) abort();
    return field ? native_version_sets[row].macos : native_version_sets[row].set;
}
'''


def segment(name, address, size, offset, filesize, maximum, initial, flags=0):
    return struct.pack("<II16s4Q4I", 0x19, 72, name, address, size, offset,
                       filesize, maximum, initial, 0, flags)


def fixture(arch, *, rebase=b"", bind=b"", extra=b"", data_flags=0, text_sections=b""):
    page = 16384 if arch == "arm64" else 4096
    commands = segment(b"__TEXT", 0, page, 0, page, 5, 5)
    if text_sections:
        assert len(text_sections) % 80 == 0
        commands = bytearray(commands)
        struct.pack_into("<I", commands, 4, 72 + len(text_sections))
        struct.pack_into("<I", commands, 64, len(text_sections) // 80)
        commands = bytes(commands) + text_sections
    commands += segment(b"__DATA", page, page, page, page, 3, 3, data_flags)
    commands += segment(b"__LINKEDIT", page * 2, page, page * 2, page, 1, 1)
    commands += struct.pack("<12I", 0x80000022, 48,
                            page * 2, len(rebase), page * 2 + 64, len(bind),
                            0, 0, 0, 0, 0, 0)
    commands += extra
    extra_count, cursor = 0, 0
    while cursor < len(extra):
        size = struct.unpack_from("<I", extra, cursor + 4)[0]
        assert size >= 8 and cursor + size <= len(extra)
        cursor += size
        extra_count += 1
    cpu = 0x100000c if arch == "arm64" else 0x1000007
    header = struct.pack("<8I", 0xfeedfacf, cpu, 0, 6,
                         4 + extra_count, len(commands), 4, 0)
    data = bytearray((header + commands).ljust(page * 3, b"\0"))
    data[page * 2:page * 2 + len(rebase)] = rebase
    data[page * 2 + 64:page * 2 + 64 + len(bind)] = bind
    struct.pack_into("<Q", data, page, 0x88)
    return data, page


class BuildVersion(ctypes.Structure):
    _fields_ = [("platform", ctypes.c_uint32), ("version", ctypes.c_uint32)]


class UUIDOffset(ctypes.Structure):
    _fields_ = [("uuid", ctypes.c_ubyte * 16), ("offset", ctypes.c_uint64),
                ("image", ctypes.c_void_p)]


class DlInfo(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("base", ctypes.c_void_p),
                ("name", ctypes.c_char_p), ("symbol", ctypes.c_void_p)]


def build_version(sdk=0x1a0500, *, platform=1, tools=b"", minimum=0x1a0000):
    return struct.pack("<6I", 0x32, 24 + len(tools), platform, minimum, sdk,
                       len(tools) // 8) + tools


def library_command(path, version, command=0xc):
    name = path.encode() + b"\0"
    size = (24 + len(name) + 7) & ~7
    return struct.pack("<6I", command, size, 24, 0, version, 0x10000) + name.ljust(size - 24, b"\0")


class NativeDyldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="native-dyld-host-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        work = Path(cls.temporary.name)
        generate_metadata_headers(ROOT, work)
        include = work / "mach-o"
        include.mkdir()
        definitions = (HEADERS / "loader.h").read_text()
        replacements = {
            "#include <mach/machine.h>": "typedef int32_t cpu_type_t, cpu_subtype_t;",
            "#include <mach/vm_prot.h>": "typedef int32_t vm_prot_t;",
            "#include <mach/machine/thread_status.h>": "",
            "#include <architecture/byte_order.h>": "",
        }
        for before, after in replacements.items():
            if definitions.count(before) != 1:
                raise AssertionError("Apple Mach-O header platform includes changed")
            definitions = definitions.replace(before, after)
        (include / "loader.h").write_text(definitions)
        shutil.copyfile(HEADERS / "nlist.h", include / "nlist.h")
        source = work / "harness.c"
        source.write_text(HARNESS)
        cls.harnesses = {}
        for arch in ("arm64", "x86_64"):
            binary = work / (arch + ".so")
            flags = ["-D__arm64__=1"] if arch == "arm64" else []
            subprocess.run(["clang-20", "-std=c11", "-shared", "-fPIC", "-O2", "-Wall", "-Werror",
                            "-Wl,-Bsymbolic", "-DNATIVE_DYLD_HOST_TEST=1",
                            *flags, "-I", str(work), "-I", str(ROOT / "userland/native/dyld"),
                            str(source), "-o", str(binary)], check=True)
            library = ctypes.CDLL(str(binary))
            library.inspect.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
            library.inspect.restype = ctypes.c_int
            library.last_error.restype = ctypes.c_char_p
            library.last_protection.restype = ctypes.c_int
            library.last_section.restype = ctypes.c_size_t
            library.last_section_size.restype = ctypes.c_ulong
            for name, arguments, result in (
                ("_dyld_get_image_header", [ctypes.c_uint32], ctypes.c_void_p),
                ("_dyld_get_image_slide", [ctypes.c_void_p], ctypes.c_ssize_t),
                ("dyld_get_active_platform", [], ctypes.c_uint32),
                ("dyld_get_program_sdk_version_token", [], ctypes.c_uint64),
                ("dyld_program_sdk_at_least", [BuildVersion], ctypes.c_bool),
                ("dyld_sdk_at_least", [ctypes.c_void_p, BuildVersion], ctypes.c_bool),
                ("NSVersionOfLinkTimeLibrary", [ctypes.c_char_p], ctypes.c_int32),
                ("_dyld_is_memory_immutable", [ctypes.c_void_p, ctypes.c_size_t], ctypes.c_bool),
                ("dyld_process_is_restricted", [], ctypes.c_bool),
                ("_dyld_get_image_uuid", [ctypes.c_void_p, ctypes.c_void_p], ctypes.c_bool),
                ("_dyld_images_for_addresses", [ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(UUIDOffset)], None),
                ("_dyld_stack_range", [ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)], None),
                ("_dyld_get_shared_cache_range", [ctypes.POINTER(ctypes.c_size_t)], ctypes.c_void_p),
                ("_dyld_get_shared_cache_uuid", [ctypes.c_void_p], ctypes.c_bool),
                ("dyld_image_header_containing_address", [ctypes.c_void_p], ctypes.c_void_p),
                ("dladdr", [ctypes.c_void_p, ctypes.POINTER(DlInfo)], ctypes.c_int),
                ("inspect_export", [ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_int),
                ("inspect_transaction", [], ctypes.c_int),
                ("set_security", [ctypes.c_int, ctypes.c_uint32, ctypes.c_int], None),
                ("security_observation", [ctypes.c_uint], ctypes.c_uint),
                ("version_map_count", [], ctypes.c_uint),
                ("version_map_field", [ctypes.c_uint, ctypes.c_uint], ctypes.c_uint32),
            ):
                function = getattr(library, name)
                function.argtypes, function.restype = arguments, result
            cls.harnesses[arch] = library

    def check(self, arch, data, *, kind=6, operation=0, error=None):
        buffer = ctypes.create_string_buffer(bytes(data))
        library = self.harnesses[arch]
        status = library.inspect(buffer, kind, operation)
        diagnostic = library.last_error().decode()
        if error is None:
            self.assertEqual(status, 0, diagnostic)
        else:
            self.assertEqual(status, 78, diagnostic)
            self.assertIn(error, diagnostic)
        return buffer

    def test_native_built_linker_headers_parse_for_each_cpu(self):
        for arch in self.harnesses:
            path = ROOT / "bazel-bin/userland/native" / ("dyld." + arch)
            with self.subTest(arch=arch):
                if not path.is_file():
                    self.skipTest("build //userland/native:dynamic_linkers for real-header regression")
                self.check(arch, path.read_bytes(), kind=7)

    def test_zero_absolute_export_is_distinct_from_missing_symbol(self):
        for arch, library in self.harnesses.items():
            page = 16384 if arch == "arm64" else 4096
            strings = b"\0_zero\0"
            metadata = struct.pack("<6I", 2, 24, page * 2 + 128, 1, page * 2 + 512, len(strings))
            data, _ = fixture(arch, extra=metadata)
            struct.pack_into("<IBBHQ", data, page * 2 + 128, 1, 3, 0, 0, 0)
            data[page * 2 + 512:page * 2 + 512 + len(strings)] = strings
            buffer = self.check(arch, data)
            self.assertIsNotNone(buffer)
            address = ctypes.c_size_t(99)
            self.assertEqual(library.inspect_export(b"_zero", ctypes.byref(address)), 1)
            self.assertEqual(address.value, 0)
            self.assertEqual(library.inspect_export(b"_missing", ctypes.byref(address)), 0)

    def test_transaction_error_returns_to_owner_without_process_exit(self):
        for arch, library in self.harnesses.items():
            data, _ = fixture(arch)
            buffer = self.check(arch, data)
            self.assertIsNotNone(buffer)
            self.assertEqual(library.inspect_transaction(), 1)
            self.assertEqual(library.last_error(), b"")

    def test_command_bounds_and_architecture_rejected(self):
        for arch in self.harnesses:
            base, _ = fixture(arch)
            cases = [(4, 0, "foreign"), (16, 257, "command area"),
                     (20, 32769, "command area"), (36, 7, "load-command size"),
                     (36, 0xfffffff8, "load-command size")]
            for offset, value, error in cases:
                with self.subTest(arch=arch, offset=offset, value=value):
                    data = bytearray(base)
                    struct.pack_into("<I", data, offset, value)
                    self.check(arch, data, error=error)
            data, _ = fixture(arch, extra=struct.pack("<4I", 0x1e, 16, 0, 0))
            self.check(arch, data)  # LC_SEGMENT_SPLIT_INFO is linker metadata.
            data, _ = fixture(arch, extra=struct.pack("<2I", 0x1e, 8))
            self.check(arch, data, error="split")

    def test_segment_protections_and_overlapping_mappings_rejected(self):
        for arch in self.harnesses:
            base, page = fixture(arch)
            for offset, value in ((32 + 56, 13), (32 + 60, 13),
                                  (32 + 56, 1), (32 + 60, 7)):
                with self.subTest(arch=arch, offset=offset, value=value):
                    data = bytearray(base)
                    struct.pack_into("<I", data, offset, value)
                    self.check(arch, data, error="protection")
            for offset, value in ((32 + 72 + 24, 0), (32 + 32, page + 1)):
                with self.subTest(arch=arch, overlap_offset=offset):
                    data = bytearray(base)
                    struct.pack_into("<Q", data, offset, value)
                    self.check(arch, data, error="overlap")

    def test_pointer_rebases_reject_invalid_targets_and_uleb(self):
        for arch in self.harnesses:
            data, page = fixture(arch, rebase=b"\x11\x21\x00\x51\x00")
            buffer = self.check(arch, data, operation=1)
            self.assertEqual(struct.unpack_from("<Q", buffer, page)[0], ctypes.addressof(buffer) + 0x88)
            cases = [(b"\x11\x20\x00\x51\x00", "fixup target"),
                     (b"\x11\x21\x01\x51\x00", "fixup target"),
                     (b"\x11\x2f\x00\x51\x00", "segment index"),
                     (b"\x11\x21\x80", "truncated ULEB128"),
                     (b"\x11\x21" + b"\xff" * 10, "ULEB128 overflow"),
                     (b"\x11", "unterminated rebase")]
            for stream, error in cases:
                with self.subTest(arch=arch, stream=stream):
                    data, _ = fixture(arch, rebase=stream)
                    self.check(arch, data, operation=1, error=error)

    def test_missing_strong_import_fails_and_missing_weak_import_is_null(self):
        for arch in self.harnesses:
            for weak in (False, True):
                with self.subTest(arch=arch, weak=weak):
                    stream = bytes([0x40 | int(weak)]) + b"_missing\0\x71\x00\x90\x00"
                    data, page = fixture(arch, bind=stream)
                    buffer = self.check(arch, data, operation=2,
                                        error=None if weak else "unresolved native import: _missing")
                    if weak:
                        self.assertEqual(struct.unpack_from("<Q", buffer, page)[0], 0)

    def test_bind_backward_deltas_validate_final_slot(self):
        def uleb(value):
            result = bytearray()
            while value >= 128:
                result.append((value & 127) | 128)
                value >>= 7
            return bytes(result + bytes([value]))

        for arch in self.harnesses:
            # ld64.lld emits this two's-complement ADD_ADDR_ULEB pattern in the
            # genuine malloc root: bindings are grouped by symbol, not address.
            prefix = b"\x41_missing\0\x71\x20\x90\x80"
            for delta, expected in ((-40, None), (-48, "fixup target"), (-39, "fixup target")):
                with self.subTest(arch=arch, delta=delta):
                    stream = prefix + uleb(delta % (2**64)) + b"\x90\0"
                    data, page = fixture(arch, bind=stream)
                    buffer = self.check(arch, data, operation=2, error=expected)
                    if expected is None:
                        self.assertEqual(struct.unpack_from("<Q", buffer, page)[0], 0)
                        self.assertEqual(struct.unpack_from("<Q", buffer, page + 32)[0], 0)

    def test_unimplemented_weak_definition_coalescing_is_rejected(self):
        for arch in self.harnesses:
            base, page = fixture(arch)
            for flag in (0x8000, 0x10000):  # MH_WEAK_DEFINES / MH_BINDS_TO_WEAK
                with self.subTest(arch=arch, flag=flag):
                    data = bytearray(base)
                    struct.pack_into("<I", data, 24, 4 | flag)
                    self.check(arch, data, error="weak-definition coalescing")
            with self.subTest(arch=arch, weak_bind_stream=True):
                data = bytearray(base)
                # A coalescing stream must also be rejected if the header flags
                # fail to announce it. Ordinary weak imports remain supported.
                struct.pack_into("<II", data, 32 + 72 * 3 + 24, page * 2, 1)
                self.check(arch, data, error="weak-definition coalescing")

    def test_data_const_is_read_only_after_fixups(self):
        for arch in self.harnesses:
            with self.subTest(arch=arch):
                data, _ = fixture(arch, data_flags=0x10)  # SG_READ_ONLY
                self.check(arch, data, operation=3)
                self.assertEqual(self.harnesses[arch].last_protection(), 1)

    def test_loaded_section_query_checks_mapping_bounds(self):
        for arch in self.harnesses:
            base, page = fixture(arch)
            section = struct.pack("<16s16sQQ8I", b"__text", b"__TEXT", 256, 16, 256, 0, 0, 0, 0, 0, 0, 0)
            data = bytearray(base[:104] + section + base[104:])
            data = data[:len(base)]
            struct.pack_into("<I", data, 20, struct.unpack_from("<I", base, 20)[0] + 80)
            struct.pack_into("<I", data, 36, 152)
            struct.pack_into("<I", data, 32 + 64, 1)
            buffer = self.check(arch, data, operation=4)
            library = self.harnesses[arch]
            self.assertEqual(library.last_section(), ctypes.addressof(buffer) + 256)
            self.assertEqual(library.last_section_size(), 16)
            missing = bytearray(data)
            missing[104:120] = b"__absent".ljust(16, b"\0")
            self.check(arch, missing, operation=4)
            self.assertEqual((library.last_section(), library.last_section_size()), (0, 0))
            for address, size in ((page - 8, 16), (0, page + 1), (2**64 - 1, 16)):
                with self.subTest(arch=arch, address=address, size=size):
                    bad = bytearray(data)
                    struct.pack_into("<QQ", bad, 104 + 32, address, size)
                    self.check(arch, bad, operation=4, error="section is outside its mapping")

    def test_platform_uuid_and_version_metadata_validation(self):
        uuid = struct.pack("<2I16s", 0x1b, 24, bytes(range(16)))
        legacy = struct.pack("<4I", 0x24, 16, 0x0a0f00, 0x0f0500)
        for arch, library in self.harnesses.items():
            for metadata, sdk in ((build_version(tools=struct.pack("<2I", 3, 0x140000)), 0x1a0500),
                                  (legacy, 0x0f0500)):
                with self.subTest(arch=arch, sdk=sdk):
                    data, _ = fixture(arch, extra=metadata + uuid)
                    buffer = self.check(arch, data)
                    address = ctypes.addressof(buffer)
                    self.assertEqual(library._dyld_get_image_header(0), address)
                    self.assertIsNone(library._dyld_get_image_header(1))
                    self.assertEqual(library._dyld_get_image_slide(address), address)
                    self.assertEqual(library._dyld_get_image_slide(1), 0)
                    self.assertEqual(library.dyld_get_active_platform(), 1)
                    self.assertEqual(library.dyld_get_program_sdk_version_token(), (sdk << 32) | 1)
                    for platform, version, expected in ((1, sdk, True), (1, sdk - 1, True),
                                                        (1, sdk + 1, False), (2, sdk, False)):
                        query = BuildVersion(platform, version)
                        self.assertEqual(library.dyld_sdk_at_least(address, query), expected)
                        self.assertEqual(library.dyld_program_sdk_at_least(query), expected)
                    self.assertFalse(library.dyld_sdk_at_least(1, BuildVersion(1, 0)))
                    value = (ctypes.c_ubyte * 16)()
                    self.assertTrue(library._dyld_get_image_uuid(address, value))
                    self.assertEqual(bytes(value), bytes(range(16)))
                    self.assertFalse(library._dyld_get_image_uuid(1, value))
                    self.assertEqual(bytes(value), bytes(16))
            malformed_tool_count = bytearray(build_version())
            struct.pack_into("<I", malformed_tool_count, 20, 1)
            cases = ((uuid + uuid, "UUID"), (struct.pack("<2I", 0x1b, 8), "UUID"),
                     (build_version() + build_version(), "build-version"),
                     (legacy + build_version(), "build-version"),
                     (build_version() + legacy, "minimum-version"),
                     (build_version(platform=2), "platform"),
                     (malformed_tool_count, "build-tool table"))
            for metadata, error in cases:
                with self.subTest(arch=arch, malformed=error):
                    data, _ = fixture(arch, extra=metadata)
                    self.check(arch, data, error=error)

    def test_availability_version_sets_use_genuine_generated_map(self):
        for arch, library in self.harnesses.items():
            rows = [(library.version_map_field(n, 0), library.version_map_field(n, 1))
                    for n in range(library.version_map_count())]
            self.assertGreater(len(rows), 10)
            # Include every source-generated boundary, its preceding SDK patch,
            # and unknown future values; no handwritten compatibility table.
            for current_set, sdk in rows:
                for actual_sdk in (sdk, sdk - 1):
                    data, _ = fixture(arch, extra=build_version(actual_sdk))
                    buffer = self.check(arch, data)
                    address = ctypes.addressof(buffer)
                    equivalent = max((version_set for version_set, mapped in rows
                                      if mapped <= actual_sdk), default=0)
                    for requested in (current_set, current_set + 1, 0xffffffff):
                        with self.subTest(arch=arch, sdk=actual_sdk, requested=requested):
                            mapped = next((mapped for version_set, mapped in rows
                                           if version_set >= requested), None)
                            query = BuildVersion(0xffffffff, requested)
                            self.assertEqual(library.dyld_sdk_at_least(address, query),
                                             mapped is not None and actual_sdk >= mapped)
                            self.assertEqual(library.dyld_program_sdk_at_least(query),
                                             equivalent >= requested)

    def test_link_time_library_versions_use_dependency_commands(self):
        commands = (library_command("/usr/lib/libSystem.B.dylib", 0x1000203) +
                    library_command("/usr/lib/libSample.dylib", 0x20304, 0x80000018) +
                    library_command("/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit", 0x50607))
        for arch, library in self.harnesses.items():
            data, _ = fixture(arch, extra=commands)
            buffer = self.check(arch, data)
            self.assertIsNotNone(buffer)
            for name, expected in ((b"System", 0x1000203), (b"Sample", 0x20304), (b"AppKit", 0x50607),
                                   (b"Missing", -1), (b"Syste", -1), (b"Sample.dylib", -1), (None, -1)):
                with self.subTest(arch=arch, name=name):
                    self.assertEqual(library.NSVersionOfLinkTimeLibrary(name), expected)

    def test_image_ranges_uuid_offsets_and_absent_cache(self):
        identifier = bytes(range(16))
        uuid = struct.pack("<2I16s", 0x1b, 24, identifier)
        for arch, library in self.harnesses.items():
            data, page = fixture(arch, extra=uuid, data_flags=0x10)
            buffer = self.check(arch, data)
            address = ctypes.addressof(buffer)
            for offset, size, expected in ((0, page, True), (0, 0, False), (page - 1, 1, True),
                                          (page - 1, 2, False), (page, 1, False),
                                          (2 * page, page, True), (3 * page, 1, False)):
                with self.subTest(arch=arch, offset=offset, size=size):
                    self.assertEqual(library._dyld_is_memory_immutable(address + offset, size), expected)
            self.assertFalse(library._dyld_is_memory_immutable(address, 2**64 - 1))
            addresses = (ctypes.c_void_p * 4)(address + 32, address + page, address + 3 * page, 1)
            results = (UUIDOffset * 4)()
            library._dyld_images_for_addresses(4, addresses, results)
            for n in range(4):
                known = n < 2
                self.assertEqual(library.dyld_image_header_containing_address(addresses[n]), address if known else None)
                self.assertEqual(results[n].image, address if known else None)
                self.assertEqual(results[n].offset, addresses[n] - address if known else 0)
                self.assertEqual(bytes(results[n].uuid), identifier if known else bytes(16))
            bottom, top, size = ctypes.c_void_p(1), ctypes.c_void_p(1), ctypes.c_size_t(1)
            library._dyld_stack_range(ctypes.byref(bottom), ctypes.byref(top))
            self.assertEqual((bottom.value, top.value), (None, None))
            self.assertIsNone(library._dyld_get_shared_cache_range(ctypes.byref(size)))
            self.assertEqual(size.value, 0)
            value = (ctypes.c_ubyte * 16)(*range(16))
            self.assertFalse(library._dyld_get_shared_cache_uuid(value))
            self.assertEqual(bytes(value), identifier)

    def test_dladdr_uses_nearest_symbol_in_same_section(self):
        section = struct.pack("<16s16sQQ8I", b"__text", b"__TEXT", 2048, 256, 2048, 0, 0, 0, 0, 0, 0, 0)
        for arch, library in self.harnesses.items():
            page = 16384 if arch == "arm64" else 4096
            strings = b"\0_local\0_global\0_nearest\0_wrong_section\0_stab\0"
            symbols = ((1, 0xe, 1, 2048), (8, 0xf, 1, 2048), (16, 0xf, 1, 2112),
                       (25, 0xf, 2, 2144), (40, 0xef, 1, 2150))
            metadata = struct.pack("<6I", 2, 24, page * 2 + 128, len(symbols), page * 2 + 512, len(strings))
            data, _ = fixture(arch, extra=metadata, text_sections=section)
            for n, (index, kind, section_index, value) in enumerate(symbols):
                struct.pack_into("<IBBHQ", data, page * 2 + 128 + n * 16, index, kind, section_index, 0, value)
            data[page * 2 + 512:page * 2 + 512 + len(strings)] = strings
            buffer = self.check(arch, data)
            address = ctypes.addressof(buffer)
            for offset, name, symbol in ((0, b"__dso_handle", 0), (1024, None, None),
                                         (2049, b"global", 2048), (2155, b"nearest", 2112),
                                         (page, None, None)):
                with self.subTest(arch=arch, offset=offset):
                    info = DlInfo()
                    self.assertEqual(library.dladdr(address + offset, ctypes.byref(info)), 1)
                    self.assertEqual(info.path, b"/test/native-image")
                    self.assertEqual(info.base, address)
                    self.assertEqual(info.name, name)
                    self.assertEqual(info.symbol, None if symbol is None else address + symbol)
            info = DlInfo(b"old", 1, b"old", 1)
            self.assertEqual(library.dladdr(1, ctypes.byref(info)), 0)
            self.assertEqual((info.path, info.base, info.name, info.symbol), (None, None, None, None))
            invalid = bytearray(data)
            struct.pack_into("<I", invalid, page * 2 + 128 + 2 * 16, len(strings))
            self.check(arch, invalid, operation=5, error="symbol name outside string table")
            invalid = bytearray(data)
            invalid[page * 2 + 512 + 16:page * 2 + 512 + len(strings)] = b"x" * (len(strings) - 16)
            self.check(arch, invalid, operation=5, error="unterminated symbol name")

    def test_security_policy_records_native_observations_and_disables_path_overrides(self):
        for arch, library in self.harnesses.items():
            data, _ = fixture(arch)
            buffer = self.check(arch, data)
            self.assertIsNotNone(buffer)
            for status, flags, changed in ((0, 0, 0), (0, 0x800, 0), (0, 0x10000, 1), (-1, 0x800, 0)):
                with self.subTest(arch=arch, status=status, flags=flags, changed=changed):
                    library.set_security(status, flags, changed)
                    self.assertTrue(library.dyld_process_is_restricted())
                    self.assertTrue(library.dyld_process_is_restricted())
                    self.assertEqual([library.security_observation(n) for n in range(6)],
                                     [1, 1, int(status == 0), flags if status == 0 else 0, changed, 0])
            page = 16384 if arch == "arm64" else 4096
            restriction = bytearray(segment(b"__RESTRICT", page * 3, page, page * 3, 0, 1, 1))
            struct.pack_into("<I", restriction, 4, 152)
            struct.pack_into("<I", restriction, 64, 1)
            restriction += struct.pack("<16s16sQQ8I", b"__restrict", b"__RESTRICT", page * 3, 0,
                                       page * 3, 0, 0, 0, 0, 0, 0, 0)
            data, _ = fixture(arch, extra=restriction)
            buffer = self.check(arch, data)
            self.assertIsNotNone(buffer)
            library.set_security(0, 0, 0)
            self.assertTrue(library.dyld_process_is_restricted())
            self.assertEqual(library.security_observation(5), 1)


if __name__ == "__main__":
    unittest.main()
