"""Exercise verifier failure paths with a simulated serial endpoint, not boot evidence."""
import os
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from verify_qemu_boot import SerialLog


ROOT = Path(__file__).resolve().parents[1]
FAKE_QEMU = '''#!/usr/bin/env python3
import os, re, sys, time
mode = os.environ["SERIAL_FIXTURE"]
if mode == "chunked":
    print("Darwin Kernel Version 25.", end="", flush=True)
    time.sleep(0.1)
    print("5.0: fixture", flush=True)
else:
    print("Darwin Kernel Version " + ("19.6.0" if mode == "wrong" else "25.5.0") + ": fixture", flush=True)
if mode == "panic":
    print("panic(cpu 0): fixture panic", flush=True)
else:
    print("-sh-3.2# ", end="", flush=True)
    if mode != "wrong":
        line = sys.stdin.readline()
        if mode == "echo":
            print(line, end="", flush=True)
        else:
            nonce = re.search(r"[0-9a-f]{48}", line).group(0)
            print("\\n" + nonce + ":25.5.0:x86_64", flush=True)
            if mode.startswith("init-"):
                print("\\nroot@darwin-embedded# ", end="", flush=True)
                line = sys.stdin.readline()
                if mode == "init-echo":
                    print(line, end="", flush=True)
                else:
                    ppid = "2" if mode == "init-wrong-parent" else "1"
                    print("\\nNATIVE_INIT_PID:" + nonce + ":42:" + ppid, flush=True)
                    print("\\nroot@darwin-embedded# ", end="", flush=True)
                    sys.stdin.readline()
                    status = "7" if mode == "init-wrong-status" else "23"
                    print("\\nNative init reaped: 42:" + status, flush=True)
                    print("\\nroot@darwin-embedded# ", end="", flush=True)
                    sys.stdin.readline()
                    pid = "42" if mode == "init-same-pid" else "43"
                    print("\\nNATIVE_INIT_PID:" + nonce + ":" + pid + ":1", flush=True)
                    print("\\nroot@darwin-embedded# ", end="", flush=True)
            if mode.startswith(("native-", "runtime-", "pthread-", "math-", "malloc-", "tls-", "shell-", "modules-", "resolver-", "graphics-", "dependencies-", "glib-", "gio-", "fonts-", "pango-", "pixbuf-", "dbus-", "xfce-")):
                print("\\nroot@darwin-embedded# ", end="", flush=True)
                line = sys.stdin.readline()
                if mode.endswith("-echo"):
                    print(line, end="", flush=True)
                elif mode.endswith("-failed") and not mode.endswith("-exit-failed"):
                    print("command failed", flush=True)
                else:
                    proof = nonce + ":25.5.0:" + ("arm64" if mode.endswith("-wrong") else "x86_64")
                    prefix = "NATIVE_RUNTIME_OK:" if mode == "runtime-proof" else "NATIVE_DYLD_OK:"
                    for kind in ("pthread", "math", "malloc", "tls", "shell", "modules", "resolver", "graphics", "dependencies", "glib", "gio", "fonts", "pango", "pixbuf", "dbus"):
                        if mode.startswith(kind + "-") and not mode.endswith("-legacy"):
                            prefix = "NATIVE_" + kind.upper() + "_OK:"
                    if mode.startswith("xfce-"):
                        prefix = "NATIVE_XFCE_BASE_OK:"
                        first = "3" if mode == "xfce-same-client" else "4"
                        second = "4" if mode == "xfce-same-daemon" else "1" if mode == "xfce-invalid-pid" else "5"
                        proof += ":client=3:first=" + first + ":second=" + second + ":rc=1:query=1:xml=1"
                    print("\\n" + prefix + proof, flush=True)
                    if mode.endswith("-exit-failed"):
                        print("command failed", flush=True)
                    if mode.endswith("-late-failure"):
                        kind = "TLS" if mode.startswith("tls-") else "GIO" if mode.startswith("gio-") else mode.split("-")[0]
                        print("Native " + kind + " failure: fixture", flush=True)
                    if mode.startswith("graphics-"):
                        print("\\nNATIVE_GRAPHICS_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                    if mode.startswith("dependencies-"):
                        print("\\nNATIVE_DEPENDENCIES_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                    if mode.startswith("glib-"):
                        print("\\nNATIVE_GLIB_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                    if mode.startswith("gio-"):
                        print("\\nNATIVE_GIO_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                    if mode.startswith("fonts-"):
                        print("\\nNATIVE_FONTS_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                        if mode=="fonts-after-exit-failure":
                            print("Native fonts failure: fixture after exit", flush=True)
                    if mode.startswith("pango-"):
                        print("\\nNATIVE_PANGO_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                        if mode=="pango-after-exit-failure":
                            print("Native pango failure: fixture after exit", flush=True)
                    if mode.startswith("pixbuf-"):
                        print("\\nNATIVE_PIXBUF_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                        if mode=="pixbuf-after-exit-failure":
                            print("Native pixbuf failure: fixture after exit", flush=True)
                    if mode.startswith("dbus-"):
                        print("\\nNATIVE_DBUS_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                        if mode=="dbus-after-exit-failure":
                            print("Native dbus failure: fixture after exit", flush=True)
                    if mode.startswith("xfce-"):
                        print("\\nNATIVE_XFCE_BASE_EXIT:" + nonce + ":" + ("7" if mode.endswith("-nonzero") else "0"), flush=True)
                        if mode == "xfce-after-exit-failure":
                            print("NATIVE_XFCE_BASE_FAIL:fixture after exit", flush=True)
                    print("\\nroot@darwin-embedded# ", end="", flush=True)
time.sleep(5)
'''


class VerifierTests(unittest.TestCase):
    def test_text_and_raw_interactive_log_output(self):
        stream = io.BytesIO()
        log = SerialLog(stream)
        log.write("Darwin 25.5.0\n")
        log.write(b"raw console\n")
        log.flush()
        self.assertEqual(stream.getvalue(), b"Darwin 25.5.0\nraw console\n")

    def run_fixture(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            emulator = work / "qemu-system-x86_64"
            emulator.write_text(FAKE_QEMU)
            emulator.chmod(0o755)
            copy = work / "mcopy"
            copy.write_text("#!/bin/sh\nexit 0\n")
            copy.chmod(0o755)
            image = work / "boot.img"
            image.touch()
            env = dict(os.environ, SERIAL_FIXTURE=mode,
                       PATH=str(work) + os.pathsep + os.environ["PATH"])
            return subprocess.run([
                sys.executable, str(ROOT / "scripts/verify_qemu_boot.py"),
                "--arch", "x86_64", "--boot-image", str(image),
                "--timeout", "1", "--output-dir", str(work / "logs"),
                *(["--native-dyld-probe"] if mode.startswith("native-") else []),
                *(["--native-runtime-probe"] if mode.startswith("runtime-") else []),
                *(["--native-pthread-probe"] if mode.startswith("pthread-") else []),
                *(["--native-math-probe"] if mode.startswith("math-") else []),
                *(["--native-malloc-probe"] if mode.startswith("malloc-") else []),
                *(["--native-tls-probe"] if mode.startswith("tls-") else []),
                *(["--native-shell-probe"] if mode.startswith("shell-") else []),
                *(["--native-modules-probe"] if mode.startswith("modules-") else []),
                *(["--native-init-probe"] if mode.startswith("init-") else []),
                *(["--native-resolver-probe"] if mode.startswith("resolver-") else []),
                *(["--native-graphics-probe"] if mode.startswith("graphics-") else []),
                *(["--native-dependencies-probe"] if mode.startswith("dependencies-") else []),
                *(["--native-glib-probe"] if mode.startswith("glib-") else []),
                *(["--native-gio-probe"] if mode.startswith("gio-") else []),
                *(["--native-fonts-probe"] if mode.startswith("fonts-") else []),
                *(["--native-pango-probe"] if mode.startswith("pango-") else []),
                *(["--native-pixbuf-probe"] if mode.startswith("pixbuf-") else []),
                *(["--native-dbus-probe"] if mode.startswith("dbus-") else []),
                *(["--native-xfce-base-probe"] if mode.startswith("xfce-") else [])],
                env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=10)

    def test_wrong_kernel_fails(self):
        result = self.run_fixture("wrong")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("wrong kernel", result.stdout)

    def test_panic_fails(self):
        result = self.run_fixture("panic")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("kernel panic", result.stdout)

    def test_echo_alone_fails(self):
        result = self.run_fixture("echo")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertNotIn("Verified interactive", result.stdout)

    def test_complete_nonce_output_passes(self):
        result = self.run_fixture("proof")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Verified interactive Darwin 25.5.0 x86_64", result.stdout)

    def test_chunked_kernel_banner_passes(self):
        result = self.run_fixture("chunked")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("Verified interactive Darwin 25.5.0 x86_64", result.stdout)

    def test_native_library_proof_requires_complete_matching_output(self):
        for mode in ("native-proof", "native-echo", "native-wrong", "native-exit-failed"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "native-proof" else 1, result.stdout)
                self.assertEqual("Verified native Mach-O shared libraries" in result.stdout,
                                 mode == "native-proof")

    def test_compiler_tls_requires_matching_proof_and_successful_exit(self):
        for mode in ("tls-proof", "tls-echo", "tls-wrong", "tls-exit-failed", "tls-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "tls-proof" else 1, result.stdout)
                self.assertEqual("Verified native compiler TLS and destructors" in result.stdout,
                                 mode == "tls-proof")

    def test_actual_shell_requires_matching_proof_and_successful_exit(self):
        for mode in ("shell-proof", "shell-echo", "shell-wrong", "shell-exit-failed", "shell-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "shell-proof" else 1, result.stdout)
                self.assertEqual("Verified actual native dash scripts and account lookup" in result.stdout,
                                 mode == "shell-proof")

    def test_dynamic_modules_require_matching_proof_and_successful_exit(self):
        for mode in ("modules-proof", "modules-echo", "modules-wrong", "modules-exit-failed", "modules-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "modules-proof" else 1, result.stdout)
                self.assertEqual("Verified native dynamic modules, unloading and concurrency" in result.stdout,
                                 mode == "modules-proof")

    def test_native_init_requires_real_parent_exit_status_and_new_process(self):
        for mode in ("init-proof", "init-echo", "init-wrong-parent", "init-wrong-status", "init-same-pid"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "init-proof" else 1, result.stdout)
                self.assertEqual("Verified native PID 1 shell reaping and respawn" in result.stdout,
                                 mode == "init-proof")

    def test_resolver_requires_matching_proof_and_successful_exit(self):
        for mode in ("resolver-proof", "resolver-echo", "resolver-wrong", "resolver-exit-failed", "resolver-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "resolver-proof" else 1, result.stdout)
                self.assertEqual("Verified native resolver and DNS lifecycle" in result.stdout,
                                 mode == "resolver-proof")

    def test_graphics_requires_matching_proof_actual_exit_status_and_no_late_failure(self):
        for mode in ("graphics-proof", "graphics-echo", "graphics-wrong", "graphics-exit-failed",
                     "graphics-nonzero", "graphics-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "graphics-proof" else 1, result.stdout)
                self.assertEqual("Verified native graphics parsing, fonts and compositing" in result.stdout,
                                 mode == "graphics-proof")

    def test_dependencies_require_matching_proof_actual_exit_and_no_late_failure(self):
        for mode in ("dependencies-proof", "dependencies-echo", "dependencies-wrong", "dependencies-exit-failed",
                     "dependencies-nonzero", "dependencies-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "dependencies-proof" else 1, result.stdout)
                self.assertEqual("Verified native regex, conversions, FFI callbacks and catalogs" in result.stdout,
                                 mode == "dependencies-proof")

    def test_glib_requires_matching_proof_actual_exit_and_no_late_failure(self):
        for mode in ("glib-proof", "glib-echo", "glib-wrong", "glib-exit-failed", "glib-nonzero", "glib-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "glib-proof" else 1, result.stdout)
                self.assertEqual("Verified native GLib objects, signals, threads, events and modules" in result.stdout,
                                 mode == "glib-proof")

    def test_gio_requires_matching_proof_actual_exit_and_no_late_failure(self):
        for mode in ("gio-proof", "gio-echo", "gio-wrong", "gio-exit-failed", "gio-nonzero", "gio-late-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "gio-proof" else 1, result.stdout)
                self.assertEqual("Verified actual native GIO operations" in result.stdout,
                                 mode == "gio-proof")

    def test_fonts_require_matching_proof_exit_and_no_failure_before_prompt(self):
        for mode in ("fonts-proof", "fonts-echo", "fonts-wrong", "fonts-exit-failed", "fonts-nonzero",
                     "fonts-late-failure", "fonts-after-exit-failure"):
            with self.subTest(mode=mode):
                result=self.run_fixture(mode)
                self.assertEqual(result.returncode,0 if mode=="fonts-proof" else 1,result.stdout)
                self.assertEqual("Verified actual native font discovery, shaping, drawing and shared memory" in result.stdout,
                                 mode=="fonts-proof")

    def test_pango_requires_matching_proof_exit_and_no_failure_before_prompt(self):
        for mode in ("pango-proof", "pango-echo", "pango-wrong", "pango-exit-failed", "pango-nonzero",
                     "pango-late-failure", "pango-after-exit-failure"):
            with self.subTest(mode=mode):
                result=self.run_fixture(mode)
                self.assertEqual(result.returncode,0 if mode=="pango-proof" else 1,result.stdout)
                self.assertEqual("Verified actual native Pango layout, boundaries, serialization and drawing" in result.stdout,
                                 mode=="pango-proof")

    def test_pixbuf_requires_matching_proof_exit_and_no_failure_before_prompt(self):
        for mode in ("pixbuf-proof", "pixbuf-echo", "pixbuf-wrong", "pixbuf-exit-failed", "pixbuf-nonzero",
                     "pixbuf-late-failure", "pixbuf-after-exit-failure"):
            with self.subTest(mode=mode):
                result=self.run_fixture(mode)
                self.assertEqual(result.returncode,0 if mode=="pixbuf-proof" else 1,result.stdout)
                self.assertEqual("Verified actual native GdkPixbuf decoding, pixels, streams and GIF animation" in result.stdout,
                                 mode=="pixbuf-proof")

    def test_dbus_requires_matching_proof_exit_and_no_failure_before_prompt(self):
        for mode in ("dbus-proof", "dbus-echo", "dbus-wrong", "dbus-exit-failed", "dbus-nonzero",
                     "dbus-late-failure", "dbus-after-exit-failure"):
            with self.subTest(mode=mode):
                result=self.run_fixture(mode)
                self.assertEqual(result.returncode,0 if mode=="dbus-proof" else 1,result.stdout)
                self.assertEqual("Verified actual native D-Bus accounts, codecs and session/system daemons" in result.stdout,
                                 mode=="dbus-proof")

    def test_runtime_proof_cannot_be_satisfied_by_loader_proof(self):
        for mode in ("runtime-proof", "runtime-legacy"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "runtime-proof" else 1, result.stdout)
                self.assertEqual("Verified native Mach IPC and platform libraries" in result.stdout,
                                 mode == "runtime-proof")

    def test_xfce_requires_matching_cpu_independent_pids_and_successful_exit(self):
        for mode in ("xfce-proof", "xfce-echo", "xfce-wrong", "xfce-nonzero", "xfce-same-client",
                     "xfce-same-daemon", "xfce-invalid-pid", "xfce-after-exit-failure"):
            with self.subTest(mode=mode):
                result = self.run_fixture(mode)
                self.assertEqual(result.returncode, 0 if mode == "xfce-proof" else 1, result.stdout)
                self.assertEqual("Verified actual native Xfce RC, independent xfconfd/query and XML persistence" in result.stdout,
                                 mode == "xfce-proof")

    def test_component_proofs_reject_echo_wrong_cpu_legacy_and_failed_exit(self):
        for kind in ("pthread", "math", "malloc"):
            for outcome in ("proof", "echo", "wrong", "legacy", "failed"):
                with self.subTest(kind=kind, outcome=outcome):
                    result = self.run_fixture(kind + "-" + outcome)
                    self.assertEqual(result.returncode, 0 if outcome == "proof" else 1, result.stdout)


if __name__ == "__main__":
    unittest.main()
