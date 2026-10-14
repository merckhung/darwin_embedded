#!/usr/bin/env python3
"""Require the requested Darwin kernel and a real serial shell command in QEMU."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time

import pexpect

ROOT = Path(__file__).resolve().parents[1]


class SerialLog:
    """pexpect sends text during matching and raw bytes during interact()."""
    def __init__(self, stream):
        self.stream = stream

    def write(self, value):
        self.stream.write(value.encode("utf-8") if isinstance(value, str) else value)

    def flush(self):
        self.stream.flush()


def qemu_path(path):
    return str(path.resolve()).replace(",", ",,")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-tools-probe',action='store_true',help='exercise installed native BusyBox files, archives and real ps aux')
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--kernel", type=Path,
                        default=ROOT / "bazel-bin/src/Kernel/xnu/12377.121.6/arm64/kernel.bin")
    parser.add_argument("--loader", type=Path,
                        default=ROOT / "bazel-bin/bootloader/rpi4/boot_current.bin")
    parser.add_argument("--layout", type=Path,
                        help="ARM flat image layout (defaults to kernel_layout.json beside the kernel)")
    parser.add_argument("--boot-image", type=Path, default=ROOT / "bazel-bin/boot_current.img")
    parser.add_argument("--root-image", type=Path)
    parser.add_argument("--root-report", type=Path,
                        help="retain and check the native root build report against the actual booted bytes")
    parser.add_argument("--interactive", action="store_true",
                        help="keep the serial console open after verification (Ctrl-] exits)")
    parser.add_argument("--native-dyld-probe", action="store_true",
                        help="also execute the guest's signed Mach-O shared-library runtime check")
    parser.add_argument("--native-runtime-probe", action="store_true",
                        help="also require the native Mach and platform library guest checks")
    parser.add_argument("--native-pthread-probe", action="store_true",
                        help="also require native thread, TLS, synchronization and cancellation checks")
    parser.add_argument("--native-math-probe", action="store_true",
                        help="also execute actual native math and floating-point environment checks")
    parser.add_argument("--native-malloc-probe", action="store_true",
                        help="also execute real allocator and C runtime initialization checks")
    parser.add_argument("--native-tls-probe", action="store_true",
                        help="also execute compiler Mach-O TLS, thread isolation and destructor checks")
    parser.add_argument("--native-shell-probe", action="store_true",
                        help="also execute actual Mach-O dash scripts and file-backed account checks")
    parser.add_argument("--native-modules-probe", action="store_true",
                        help="also execute real dynamic module binding, unloading and concurrency checks")
    parser.add_argument("--native-init-probe", action="store_true",
                        help="also require native PID 1 to reap and respawn the actual interactive shell")
    parser.add_argument("--native-resolver-probe", action="store_true",
                        help="also execute actual native file/numeric/DNS resolver and lifecycle checks")
    parser.add_argument("--native-graphics-probe", action="store_true",
                        help="also execute actual compression/XML/PNG/font/Pixman checks under native dash")
    parser.add_argument("--native-dependencies-probe", action="store_true",
                        help="also execute native PCRE2/iconv/FFI callbacks/gettext catalog checks")
    parser.add_argument("--native-glib-probe", action="store_true",
                        help="also execute native GLib/GObject/main-loop/thread/module checks")
    parser.add_argument("--native-gio-probe", action="store_true",
                        help="also execute actual native GIO files/events/IPC/resources/settings/process checks")
    parser.add_argument("--native-fonts-probe", action="store_true",
                        help="also execute native font discovery/cache, shaping, drawing and shared-memory checks")
    parser.add_argument("--native-pango-probe", action="store_true",
                        help="also execute native Pango layout, bidi/cursor/Thai breaks, serialization and glyph drawing")
    parser.add_argument("--native-pixbuf-probe", action="store_true",
                        help="also execute native image decoding/streaming, pixel operations and original animated GIF frames")
    parser.add_argument("--native-dbus-probe", action="store_true",
                        help="also execute native group APIs, message codecs and actual session/system bus daemons")
    parser.add_argument("--native-xfce-base-probe", action="store_true",
                        help="also execute original Xfce RC, configuration service, query and XML persistence checks")
    parser.add_argument("--native-pty-probe", action="store_true",
                        help="also require a genuine Darwin PTY and interactive Mach-O shell")
    parser.add_argument("--native-cxx-system-probe", action="store_true",
                        help="also require real LLVM int128 arithmetic and Darwin filesystem/clock APIs")
    parser.add_argument("--native-cxx-runtime-probe", action="store_true",
                        help="also require real native C++ exceptions, RTTI, threads, TLS and filesystem operations")
    parser.add_argument("--trace-cxx-unwind",action="store_true",help="enable original libunwind diagnostic logging for the C++ probe")
    parser.add_argument("--expected-version", default="25.5.0")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".cache/qemu")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.interactive and not (sys.stdin.isatty() and sys.stdout.isatty()):
        parser.error("--interactive requires a terminal")
    files = [args.kernel, args.loader] if args.arch == "arm64" else [args.boot_image]
    if args.root_image:
        files.append(args.root_image)
    if args.root_report:
        if not args.root_image:
            parser.error("--root-report requires --root-image")
        files.append(args.root_report)
    for path in files:
        if not path.is_file():
            parser.error(f"missing artifact: {path}")
    if args.root_image and args.root_image.stat().st_size != 64 * 1024 * 1024:
        parser.error("the current loaders use a 64 MiB memory disk; supply that exact size")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=args.arch + "-", dir=args.output_dir))
    # A parallel Bazel build may replace its outputs during a guest run. Boot
    # retained copies, then hash those exact bytes before launching QEMU.
    input_directory = work / "inputs"
    input_directory.mkdir()
    sources = {"root_image": args.root_image} if args.root_image else {}
    if args.root_report:
        sources["root_report"] = args.root_report
    if args.arch == "arm64":
        sources.update(kernel=args.kernel, loader=args.loader,
                       layout=args.layout or args.kernel.with_name("kernel_layout.json"))
    else:
        sources.update(boot_image=args.boot_image,
                       firmware_code=ROOT / "bazel-bin/bootloader/ovmf/OVMF_CODE_4M.fd",
                       firmware_variables=ROOT / "bazel-bin/bootloader/ovmf/OVMF_VARS-1024x768.fd")
    retained, input_records = {}, {}
    for name, source in sources.items():
        destination = input_directory / (name + source.suffix)
        shutil.copyfile(source, destination)
        destination.chmod(0o444)
        retained[name] = destination
        input_records[name] = {"source": str(source.resolve()),
                               "retained": str(destination.resolve()),
                               "size": destination.stat().st_size,
                               "sha256": hashlib.sha256(destination.read_bytes()).hexdigest()}
        if hasattr(args, name):
            setattr(args, name, destination)
    if args.root_report:
        root_report = json.loads(args.root_report.read_text())
        if (root_report["architecture"] != args.arch or
                root_report["root_sha256"] != input_records["root_image"]["sha256"]):
            parser.error("native root/report architecture or digest mismatch")
    manifest = {"architecture": args.arch, "expected_darwin": args.expected_version,
                "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "inputs": input_records,
                "entropy": "Fresh per-boot entropy is injected separately and deleted after the run."}
    manifest_path = work / "input-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"Input manifest: {manifest_path}", flush=True)
    seed = work / "entropy.bin"
    seed.write_bytes(secrets.token_bytes(256))
    seed.chmod(0o600)
    private_image = None
    if args.arch == "arm64":
        layout_path = args.layout or args.kernel.with_name("kernel_layout.json")
        try:
            layout = json.loads(layout_path.read_text())
            if layout["architecture"] != "arm64" or layout["image_size"] != args.kernel.stat().st_size:
                raise ValueError("architecture or image size mismatch")
            kernel_address = layout["physical_base"]
            if not isinstance(kernel_address, int) or not 0x100000 <= kernel_address < 0x07f00000:
                raise ValueError("kernel load address is outside the reserved image area")
        except (OSError, ValueError, KeyError, TypeError) as error:
            seed.unlink(missing_ok=True)
            parser.error(f"invalid ARM image layout: {error}")
        command = "qemu-system-aarch64"
        options = ["-M", "raspi4b", "-cpu", "cortex-a72", "-m", "2G",
                   "-device", f"loader,file={qemu_path(args.loader)},addr=0x40080000,cpu-num=0",
                   "-device", "loader,addr=0x40080000,cpu-num=1",
                   "-device", "loader,addr=0x40080000,cpu-num=2",
                   "-device", "loader,addr=0x40080000,cpu-num=3",
                   "-device", f"loader,file={qemu_path(args.kernel)},addr={kernel_address:#x}",
                   "-device", f"loader,file={qemu_path(seed)},addr=0x07f00000"]
        if args.root_image:
            options += ["-device", f"loader,file={qemu_path(args.root_image)},addr=0x0a000000"]
    else:
        private_image = work / "boot.img"
        shutil.copyfile(args.boot_image, private_image)
        private_image.chmod(0o600)
        provision = subprocess.run(["mcopy", "-o", "-i", str(private_image) + "@@1048576",
                                    str(seed), "::EFI/BOOT/entropy.bin"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if provision.returncode:
            seed.unlink()
            private_image.unlink()
            parser.error("cannot inject per-boot entropy into the ESP: " + provision.stderr.strip())
        if args.root_image:
            provision = subprocess.run(["mcopy", "-o", "-i", str(private_image) + "@@1048576",
                                        str(args.root_image), "::ramdisk.img"], capture_output=True, text=True)
            if provision.returncode:
                seed.unlink()
                private_image.unlink()
                parser.error("cannot inject the root memory disk: " + provision.stderr.strip())
        variables = work / "OVMF_VARS.fd"
        shutil.copy2(retained["firmware_variables"], variables)
        variables.chmod(0o600)
        command = "qemu-system-x86_64"
        options = ["-m", "4G", "-machine", "q35,accel=tcg",
                   "-cpu", "Penryn,vendor=GenuineIntel,vmware-cpuid-freq=on",
                   "-drive", "if=pflash,format=raw,readonly=on,file=" +
                   qemu_path(retained["firmware_code"]),
                   "-drive", f"if=pflash,format=raw,file={qemu_path(variables)}",
                   "-smbios", "type=2", "-device", "virtio-vga",
                   "-drive", f"file={qemu_path(private_image)},format=raw,snapshot=on"]
    options += ["-serial", "stdio", "-monitor", "none", "-display", "none", "-no-reboot"]
    log_path = work / "serial.log"
    print(f"Serial log: {log_path}", flush=True)
    deadline = time.monotonic() + args.timeout
    child = None
    try:
        with log_path.open("wb") as log:
            child = pexpect.spawn(command, options, encoding="utf-8", codec_errors="replace",
                                  echo=False, timeout=10)
            child.logfile_read = SerialLog(log)
            kernel_seen = False
            while time.monotonic() < deadline:
                remaining = min(10, deadline - time.monotonic())
                if remaining <= 0:
                    break
                found = child.expect([
                    r"Darwin Kernel Version ([0-9.]+):",
                    r"(?:-sh-[0-9.]+# |(?:bash|sh)-[0-9.]+[$#] |(?:\r?\n)root[^\r\n]*# )",
                    r"panic\(cpu|MACH Reboot|Missing.*per-boot entropy|OCB: StartImage failed|Native init failure:",
                    pexpect.TIMEOUT, pexpect.EOF], timeout=remaining)
                if found == 0:
                    version = child.match.group(1)
                    if version != args.expected_version:
                        raise RuntimeError(f"wrong kernel: Darwin {version}, expected {args.expected_version}")
                    kernel_seen = True
                    print(f"Darwin {version} reached; waiting for userland", flush=True)
                elif found == 1 and kernel_seen:
                    nonce = secrets.token_hex(24)
                    child.sendline(f'printf "\\n{nonce}:%s:%s\\n" "$(uname -r)" "$(uname -m)"')
                    proof = re.escape(f"{nonce}:{args.expected_version}:{args.arch}")
                    # An anchored complete output line cannot match the echoed command.
                    child.expect(r"\r*\n" + proof + r"\r*\n",
                                 timeout=max(0.1, min(60, deadline - time.monotonic())))
                    print(f"Verified interactive Darwin {args.expected_version} {args.arch}", flush=True)
                    if args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or args.native_malloc_probe:
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-loader-test {nonce}")
                        prefix = ("NATIVE_MALLOC_OK:" if args.native_malloc_probe else
                                  "NATIVE_PTHREAD_OK:" if args.native_pthread_probe else
                                  "NATIVE_RUNTIME_OK:" if args.native_runtime_probe else "NATIVE_DYLD_OK:")
                        checked = child.expect([
                            r"\r*\n" + prefix + proof + r"\r*\n",
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native runtime failure: [^\r\n]*\r*\n",
                            r"command failed"],
                            timeout=max(0.1, min(60, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native library check rejected: " + child.after.strip())
                        finished = child.expect([r"command failed", r"\r*\nroot[^\r\n]*# "],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished == 0:
                            raise RuntimeError("native library check did not exit successfully")
                        kind = ("allocator and C runtime startup" if args.native_malloc_probe else
                                "pthread lifecycle and cancellation" if args.native_pthread_probe else
                                "Mach IPC and platform libraries" if args.native_runtime_probe else "Mach-O shared libraries")
                        print(f"Verified native {kind} on {args.arch}", flush=True)
                    if args.native_math_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or args.native_malloc_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-math-test {nonce}")
                        checked = child.expect([
                            r"\r*\nNATIVE_MATH_OK:" + proof + r"\r*\n",
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native math failure: [^\r\n]*\r*\n",
                            r"command failed"],
                            timeout=max(0.1, min(60, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native math check rejected: " + child.after.strip())
                        finished = child.expect([r"command failed", r"\r*\nroot[^\r\n]*# "],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished == 0:
                            raise RuntimeError("native math check did not exit successfully")
                        print(f"Verified native math and floating-point environment on {args.arch}", flush=True)
                    if args.native_tls_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-tls-test {nonce}")
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:TLS|runtime) failure: [^\r\n]*\r*\n",
                            r"command failed",
                        ]
                        checked = child.expect([
                            r"\r*\nNATIVE_TLS_OK:" + proof + r"\r*\n", *failures],
                            timeout=max(0.1, min(60, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native compiler TLS check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native compiler TLS check did not exit successfully: " + child.after.strip())
                        print(f"Verified native compiler TLS and destructors on {args.arch}", flush=True)
                    if args.native_resolver_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-resolver-test {nonce}")
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:resolver|runtime) failure: [^\r\n]*\r*\n",
                            r"command failed",
                        ]
                        checked = child.expect([
                            r"\r*\nNATIVE_RESOLVER_OK:" + proof + r"\r*\n", *failures],
                            timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native resolver check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native resolver check did not exit successfully: " + child.after.strip())
                        print(f"Verified native resolver and DNS lifecycle on {args.arch}", flush=True)
                    if args.native_graphics_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-graphics-test {nonce}; native_graphics_status=$?; '
                                       f'printf "\\nNATIVE_GRAPHICS_EXIT:{nonce}:%s\\n" "$native_graphics_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:graphics|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_GRAPHICS_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_GRAPHICS_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native graphics check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_GRAPHICS_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native graphics check did not exit successfully: " + child.after.strip())
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        print(f"Verified native graphics parsing, fonts and compositing on {args.arch}", flush=True)
                    if args.native_dependencies_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-dependencies-test {nonce}; native_dependency_status=$?; '
                                       f'printf "\\nNATIVE_DEPENDENCIES_EXIT:{nonce}:%s\\n" "$native_dependency_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:dependencies|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_DEPENDENCIES_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_DEPENDENCIES_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native dependency check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_DEPENDENCIES_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native dependency check did not exit successfully: " + child.after.strip())
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        print(f"Verified native regex, conversions, FFI callbacks and catalogs on {args.arch}", flush=True)
                    if args.native_glib_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-glib-test {nonce}; native_glib_status=$?; '
                                       f'printf "\\nNATIVE_GLIB_EXIT:{nonce}:%s\\n" "$native_glib_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:glib|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_GLIB_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_GLIB_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native GLib check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_GLIB_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native GLib check did not exit successfully: " + child.after.strip())
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        print(f"Verified native GLib objects, signals, threads, events and modules on {args.arch}", flush=True)
                    if args.native_gio_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or args.native_glib_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-gio-test {nonce}; native_gio_status=$?; '
                                       f'printf "\\nNATIVE_GIO_EXIT:{nonce}:%s\\n" "$native_gio_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:GIO|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_GIO_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_GIO_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native GIO check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_GIO_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native GIO check did not exit successfully: " + child.after.strip())
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        print(f"Verified actual native GIO operations on {args.arch}", flush=True)
                    if args.native_fonts_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or
                                args.native_glib_probe or args.native_gio_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-fonts-test {nonce}; native_fonts_status=$?; '
                                       f'printf "\\nNATIVE_FONTS_EXIT:{nonce}:%s\\n" "$native_fonts_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:fonts|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_FONTS_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_FONTS_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native fonts check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_FONTS_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native fonts check did not exit successfully: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures], timeout=10)
                        if finished:
                            raise RuntimeError("native fonts emitted a failure after exit: " + child.after.strip())
                        print(f"Verified actual native font discovery, shaping, drawing and shared memory on {args.arch}", flush=True)
                    if args.native_pango_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or
                                args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-pango-test {nonce}; native_pango_status=$?; '
                                       f'printf "\\nNATIVE_PANGO_EXIT:{nonce}:%s\\n" "$native_pango_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:pango|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_PANGO_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_PANGO_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native Pango check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_PANGO_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native Pango check did not exit successfully: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures], timeout=10)
                        if finished:
                            raise RuntimeError("native Pango emitted a failure after exit: " + child.after.strip())
                        print(f"Verified actual native Pango layout, boundaries, serialization and drawing on {args.arch}", flush=True)
                    if args.native_pixbuf_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or
                                args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-pixbuf-test {nonce}; native_pixbuf_status=$?; '
                                       f'printf "\\nNATIVE_PIXBUF_EXIT:{nonce}:%s\\n" "$native_pixbuf_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:pixbuf|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_PIXBUF_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_PIXBUF_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native GdkPixbuf check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_PIXBUF_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native GdkPixbuf check did not exit successfully: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures], timeout=10)
                        if finished:
                            raise RuntimeError("native GdkPixbuf emitted a failure after exit: " + child.after.strip())
                        print(f"Verified actual native GdkPixbuf decoding, pixels, streams and GIF animation on {args.arch}", flush=True)
                    if args.native_dbus_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or
                                args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe or args.native_pixbuf_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'native-dbus-test {nonce}; native_dbus_status=$?; '
                                       f'printf "\\nNATIVE_DBUS_EXIT:{nonce}:%s\\n" "$native_dbus_status"')
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:dbus|runtime|init) failure: [^\r\n]*\r*\n",
                            r"command failed",
                            r"\r*\nNATIVE_DBUS_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n",
                        ]
                        checked = child.expect([r"\r*\nNATIVE_DBUS_OK:" + proof + r"\r*\n", *failures],
                                               timeout=max(0.1, min(90, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native D-Bus check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nNATIVE_DBUS_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native D-Bus check did not exit successfully: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures], timeout=10)
                        if finished:
                            raise RuntimeError("native D-Bus emitted a failure after exit: " + child.after.strip())
                        print(f"Verified actual native D-Bus accounts, codecs and session/system daemons on {args.arch}", flush=True)
                    if args.native_xfce_base_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or
                                args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe or
                                args.native_pixbuf_probe or args.native_dbus_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'/usr/bin/dbus-run-session -- /bin/native-xfce-base-test {nonce}; '
                                       f'printf "\\nNATIVE_XFCE_BASE_EXIT:{nonce}:%s\\n" "$?"')
                        failures = [r"NATIVE_XFCE_BASE_FAIL:[^\r\n]+", r"native dyld: [^\r\n]+",
                                    r"panic\(cpu", r"CODE SIGNING: process[^\r\n]*rejecting invalid page",
                                    r"\r*\nNATIVE_XFCE_BASE_EXIT:" + re.escape(nonce) + r":[1-9][0-9]*\r*\n"]
                        pattern = (r"\r*\nNATIVE_XFCE_BASE_OK:" + proof +
                                   r":client=([0-9]+):first=([0-9]+):second=([0-9]+):rc=1:query=1:xml=1\r*\n")
                        if child.expect([pattern, *failures], timeout=max(0.1, min(120, deadline - time.monotonic()))):
                            raise RuntimeError("native Xfce base check rejected: " + child.after.strip())
                        client_pid, first_pid, second_pid = map(int, child.match.groups())
                        if min(client_pid, first_pid, second_pid) <= 1 or len({client_pid, first_pid, second_pid}) != 3:
                            raise RuntimeError("Xfce configuration daemons must be independent real processes")
                        if child.expect([r"\r*\nNATIVE_XFCE_BASE_EXIT:" + re.escape(nonce) + r":0\r*\n", *failures], timeout=20):
                            raise RuntimeError("native Xfce base check did not exit successfully: " + child.after.strip())
                        if child.expect([r"\r*\nroot[^\r\n]*# ", *failures], timeout=10):
                            raise RuntimeError("native Xfce base emitted a failure after exit: " + child.after.strip())
                        (work / "xfce-base.json").write_text(json.dumps({"architecture": args.arch, "nonce": nonce,
                            "darwin": args.expected_version, "client_pid": client_pid, "first_daemon_pid": first_pid,
                            "second_daemon_pid": second_pid, "rc": True, "independent_query": True,
                            "xml_persistence_after_restart": True, "exit_status": 0, "desktop_checked": False}, indent=2) + "\n")
                        print(f"Verified actual native Xfce RC, independent xfconfd/query and XML persistence on {args.arch}", flush=True)
                    if args.native_modules_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe or args.native_pixbuf_probe or args.native_dbus_probe or args.native_xfce_base_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-modules-test {nonce}")
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:modules|runtime) failure: [^\r\n]*\r*\n",
                            r"command failed",
                        ]
                        checked = child.expect([
                            r"\r*\nNATIVE_MODULES_OK:" + proof + r"\r*\n", *failures],
                            timeout=max(0.1, min(60, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native module check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native module check did not exit successfully: " + child.after.strip())
                        print(f"Verified native dynamic modules, unloading and concurrency on {args.arch}", flush=True)
                    if args.native_shell_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_modules_probe or args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe or args.native_pixbuf_probe or args.native_dbus_probe or args.native_xfce_base_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f"native-shell-test {nonce}")
                        failures = [
                            r"CODE SIGNING: process[^\r\n]*rejecting invalid page[^\r\n]*\r*\n",
                            r"native dyld: [^\r\n]*\r*\n",
                            r"Native (?:shell|runtime) failure: [^\r\n]*\r*\n",
                            r"command failed",
                        ]
                        checked = child.expect([
                            r"\r*\nNATIVE_SHELL_OK:" + proof + r"\r*\n", *failures],
                            timeout=max(0.1, min(60, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native shell check rejected: " + child.after.strip())
                        finished = child.expect([r"\r*\nroot[^\r\n]*# ", *failures],
                                                timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if finished:
                            raise RuntimeError("native shell check did not exit successfully: " + child.after.strip())
                        print(f"Verified actual native dash scripts and account lookup on {args.arch}", flush=True)
                    if args.native_init_probe:
                        if not (args.native_dyld_probe or args.native_runtime_probe or args.native_pthread_probe or
                                args.native_malloc_probe or args.native_math_probe or args.native_tls_probe or
                                args.native_modules_probe or args.native_shell_probe or args.native_resolver_probe or args.native_graphics_probe or args.native_dependencies_probe or args.native_glib_probe or args.native_gio_probe or args.native_fonts_probe or args.native_pango_probe or args.native_pixbuf_probe or args.native_dbus_probe or args.native_xfce_base_probe):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        failures = [r"Native init failure: [^\r\n]*\r*\n",
                                    r"native dyld: [^\r\n]*\r*\n", r"command failed"]
                        command = f'printf "\\nNATIVE_INIT_PID:{nonce}:%s:%s\\n" "$$" "$PPID"'
                        pattern = r"\r*\nNATIVE_INIT_PID:" + re.escape(nonce) + r":([0-9]+):([0-9]+)\r*\n"
                        child.sendline(command)
                        checked = child.expect([pattern, *failures],
                                               timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native init check rejected: " + child.after.strip())
                        old_pid, parent_pid = map(int, child.match.groups())
                        if old_pid <= 1 or parent_pid != 1:
                            raise RuntimeError("interactive shell is not a child of native PID 1")
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline("exit 23")
                        checked = child.expect([
                            r"\r*\nNative init reaped: " + str(old_pid) + r":23\r*\n", *failures],
                            timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native init did not reap the shell's actual exit status")
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(command)
                        checked = child.expect([pattern, *failures],
                                               timeout=max(0.1, min(10, deadline - time.monotonic())))
                        if checked:
                            raise RuntimeError("native init did not respawn a usable shell")
                        new_pid, parent_pid = map(int, child.match.groups())
                        if new_pid <= 1 or new_pid == old_pid or parent_pid != 1:
                            raise RuntimeError("native init respawn did not create a new shell process")
                        child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        print(f"Verified native PID 1 shell reaping and respawn on {args.arch}", flush=True)
                    if args.native_pty_probe:
                        if not any(value for name,value in vars(args).items() if name.startswith("native_") and name!="native_pty_probe"):
                            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                        child.sendline(f'/bin/native-pty-test {nonce}; printf "\\nNATIVE_PTY_EXIT:{nonce}:%s\\n" "$?"')
                        failures=[r"Native PTY failure: [^\r\n]+",r"native dyld: [^\r\n]+",r"panic\(cpu"]
                        if child.expect([r"\r*\nNATIVE_PTY_OK:"+proof+r"\r*\n",*failures],timeout=90):
                            raise RuntimeError("native PTY check failed: "+child.after)
                        child.expect(r"\r*\nNATIVE_PTY_EXIT:"+re.escape(nonce)+r":(\d+)\r*\n",timeout=10)
                        if child.match.group(1)!="0":raise RuntimeError("native PTY consumer returned failure")
                        child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
                        (work/"pty-result.json").write_text(json.dumps({"architecture":args.arch,"nonce":nonce,"darwin":args.expected_version,"pty_device":True,"termios":True,"window_size":[103,37],"interactive_native_shell":True,"shell_output_nonce":True,"exit_status":0},indent=2)+"\n")
                        print(f"Verified native Darwin PTY and interactive shell on {args.arch}",flush=True)
                    if args.native_cxx_system_probe:
                        if not any(value for name,value in vars(args).items() if name.startswith("native_") and name!="native_cxx_system_probe"):
                            child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
                        child.sendline(f'/bin/native-cxx-system-test {nonce}; printf "\\nNATIVE_CXX_SYSTEM_EXIT:{nonce}:%s\\n" "$?"')
                        failures=[r"Native C\+\+ system failure: [^\r\n]+",r"native dyld: [^\r\n]+",r"panic\(cpu"]
                        if child.expect([r"\r*\nNATIVE_CXX_SYSTEM_OK:"+proof+r"\r*\n",*failures],timeout=90):
                            raise RuntimeError("native C++ system check failed: "+child.after)
                        child.expect(r"\r*\nNATIVE_CXX_SYSTEM_EXIT:"+re.escape(nonce)+r":(\d+)\r*\n",timeout=10)
                        if child.match.group(1)!="0":raise RuntimeError("native C++ system consumer returned failure")
                        child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
                        (work/"cxx-system-result.json").write_text(json.dumps({"architecture":args.arch,"nonce":nonce,"darwin":args.expected_version,"int128_signed_division":True,"int128_unsigned_division":True,"unlinkat_files_and_directories":True,"monotonic_clock":True,"exit_status":0,"cxx_exceptions_checked":False},indent=2)+"\n")
                        print(f"Verified native C++ arithmetic and Darwin filesystem/clock APIs on {args.arch}",flush=True)
                    if args.native_cxx_runtime_probe:
                        if not any(value for name,value in vars(args).items() if name.startswith("native_") and name!="native_cxx_runtime_probe"):
                            child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
                        trace="LIBUNWIND_PRINT_UNWINDING=1 LIBUNWIND_PRINT_APIS=1 " if args.trace_cxx_unwind else ""
                        child.sendline(f'{trace}/bin/native-cxx-test {nonce}; printf "\\nNATIVE_CXX_RUNTIME_EXIT:{nonce}:%s\\n" "$?"')
                        failures=[r"Native C\+\+ runtime failure: [^\r\n]+",r"native dyld: [^\r\n]+",r"panic\(cpu",r"\r*\nNATIVE_CXX_RUNTIME_EXIT:"+re.escape(nonce)+r":[1-9]\d*\r*\n"]
                        if child.expect([r"\r*\nNATIVE_CXX_RUNTIME_OK:"+proof+r"\r*\n",*failures],timeout=120):
                            raise RuntimeError("native C++ runtime check failed: "+child.after)
                        child.expect(r"\r*\nNATIVE_CXX_RUNTIME_EXIT:"+re.escape(nonce)+r":(\d+)\r*\n",timeout=10)
                        if child.match.group(1)!="0":raise RuntimeError("native C++ consumer returned failure")
                        child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
                        (work/"cxx-runtime-result.json").write_text(json.dumps({"architecture":args.arch,"nonce":nonce,"darwin":args.expected_version,"cross_image_exception_unwinding":True,"rtti":True,"thread_future_timed_wait":True,"thread_local_isolation":True,"filesystem_stream_copy":True,"regex_and_float_conversion":True,"exit_status":0},indent=2)+"\n")
                        print(f"Verified native C++ exceptions, RTTI, threads, TLS and filesystem on {args.arch}",flush=True)
                    if args.native_tools_probe:
                        from verify_native_tools import verify_tools
                        verify_tools(child,work,args.arch,nonce)
                    if args.interactive:
                        print("Serial console open; Ctrl-] exits.", flush=True)
                        child.interact()
                    return 0
                elif found == 2:
                    try:
                        child.expect(pexpect.EOF, timeout=3)
                    except (pexpect.TIMEOUT, pexpect.EOF):
                        pass
                    raise RuntimeError("kernel panic or boot loader failure")
                elif found == 4:
                    raise RuntimeError("QEMU exited before the userland proof")
            raise RuntimeError("boot timed out before the userland proof")
    except (OSError, RuntimeError, pexpect.TIMEOUT, pexpect.EOF) as error:
        print(f"Boot verification failed: {error}; see {log_path}", file=sys.stderr)
        return 1
    finally:
        if child is not None:
            child.terminate(force=True)
        seed.unlink(missing_ok=True)
        if private_image:
            private_image.unlink(missing_ok=True)


if __name__ == "__main__":
    sys.exit(main())
