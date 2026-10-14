#!/usr/bin/env python3
"""Boot an isolated native framebuffer driver and capture its actual QEMU scanout."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import time

import pexpect

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def qpath(path):
    return str(path.resolve()).replace(",", ",,")


def serial_line(pattern):
    # PE console switching emits LF followed by CR before the next line.
    # Leave the terminator available as the next line's leading boundary.
    return r"\r*\n\r*" + pattern + r"(?=\r*\n)"


def qmp_command(stream, command, arguments=None):
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write((json.dumps(request) + "\n").encode())
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP closed before " + command)
        response = json.loads(line)
        if "error" in response:
            raise RuntimeError("QMP " + command + ": " + json.dumps(response))
        if "return" in response:
            return response["return"]


def check_screenshot(path, width, height):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header or tuple(map(int, header.groups())) != (width, height):
        raise RuntimeError("screenshot is not the actual reported framebuffer geometry")
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated QMP screenshot")
    # Test interior pixels across all seven stripes; console text cannot pass
    # this check, and the guest has independently read back the mapped memory.
    left, top, extent_x, extent_y = width // 4, height // 4, width // 2, height // 2
    colors = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0],
              [0, 255, 255], [255, 0, 255], [255, 255, 255]]
    samples = []
    for stripe in range(7):
        x = left + (2 * stripe + 1) * extent_x // 14
        for y in (top + extent_y // 4, top + extent_y // 2, top + 3 * extent_y // 4):
            offset = (y * width + x) * 3
            actual = list(pixels[offset:offset + 3])
            expected = colors[stripe]
            if actual != expected:
                raise RuntimeError(f"scanout pixel ({x},{y}) {actual} != mapped pattern {expected}")
            samples.append({"x": x, "y": y, "rgb": actual})
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".cache/qemu-framebuffer")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=args.arch + "-", dir=args.output_dir)).resolve()
    package = ROOT / "bazel-bin/userland/native/framebuffer"
    sources = {"root": package / f"root.{args.arch}.img", "driver_report": package / f"driver.{args.arch}.json",
               "root_report": package / f"root.{args.arch}.json"}
    if args.arch == "arm64":
        sources.update(kernel=package / "arm64/kernel.bin", layout=package / "arm64/kernel_layout.json",
                       loader=package / "arm64/boot_current.bin")
    else:
        sources.update(boot=package / "boot.x86_64.img", firmware=ROOT / "bazel-bin/bootloader/ovmf/OVMF_CODE_4M.fd",
                       variables=ROOT / "bazel-bin/bootloader/ovmf/OVMF_VARS-1024x768.fd")
    inputs, records = {}, {}
    for name, source in sources.items():
        destination = work / (name + source.suffix)
        shutil.copyfile(source, destination)
        destination.chmod(0o444)
        inputs[name] = destination
        records[name] = {"source": str(source.resolve()), "retained": str(destination), "sha256": sha(destination)}
    manifest = {"architecture": args.arch, "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "inputs": records, "scope": "Native IOUserClient framebuffer memory and actual scanout; not Xorg or Xfce."}
    (work / "input-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    seed = work / "entropy.bin"
    seed.write_bytes(secrets.token_bytes(256))
    seed.chmod(0o600)
    if args.arch == "arm64":
        layout = json.loads(inputs["layout"].read_text())
        if layout["architecture"] != "arm64" or layout["image_size"] != inputs["kernel"].stat().st_size:
            raise RuntimeError("invalid native ARM kernel layout")
        command = "qemu-system-aarch64"
        options = ["-M", "raspi4b", "-cpu", "cortex-a72", "-m", "2G",
                   "-device", f"loader,file={qpath(inputs['loader'])},addr=0x40080000,cpu-num=0"]
        for cpu in (1, 2, 3):
            options += ["-device", f"loader,addr=0x40080000,cpu-num={cpu}"]
        options += ["-device", f"loader,file={qpath(inputs['kernel'])},addr={layout['physical_base']:#x}",
                    "-device", f"loader,file={qpath(seed)},addr=0x07f00000",
                    "-device", f"loader,file={qpath(inputs['root'])},addr=0x0a000000"]
    else:
        mutable = work / "boot-private.img"
        shutil.copyfile(inputs["boot"], mutable)
        mutable.chmod(0o600)
        subprocess.run(["mcopy", "-o", "-i", str(mutable) + "@@1048576", str(seed), "::EFI/BOOT/entropy.bin"], check=True)
        variables = work / "variables-private.fd"
        shutil.copyfile(inputs["variables"], variables)
        variables.chmod(0o600)
        command = "qemu-system-x86_64"
        options = ["-m", "4G", "-machine", "q35,accel=tcg", "-cpu", "Penryn,vendor=GenuineIntel,vmware-cpuid-freq=on",
                   "-drive", "if=pflash,format=raw,readonly=on,file=" + qpath(inputs["firmware"]),
                   "-drive", "if=pflash,format=raw,file=" + qpath(variables), "-smbios", "type=2",
                   "-device", "virtio-vga", "-drive", "file=" + qpath(mutable) + ",format=raw,snapshot=on"]
    qmp_path = work / "qmp.sock"
    options += ["-serial", "stdio", "-monitor", "none", "-display", "none", "-no-reboot",
                "-qmp", "unix:" + qpath(qmp_path) + ",server=on,wait=off"]
    nonce = secrets.token_hex(24)
    proof = re.escape(nonce + ":25.5.0:" + args.arch)
    child = None
    print("Framebuffer proof directory: " + str(work), flush=True)
    try:
        with (work / "serial.log").open("w") as log:
            child = pexpect.spawn(command, options, encoding="utf-8", codec_errors="replace", echo=False, timeout=args.timeout)
            child.logfile_read = log
            child.expect(r"Darwin Kernel Version ([0-9.]+):")
            if child.match.group(1) != "25.5.0":
                raise RuntimeError("incorrect guest Darwin version")
            child.expect(r"\r*\nroot[^\r\n]*# ")
            child.sendline(f'printf "\\n{nonce}:%s:%s\\n" "$(uname -r)" "$(uname -m)"')
            child.expect(r"\r*\n" + proof + r"\r*\n", timeout=60)
            child.expect(r"\r*\nroot[^\r\n]*# ")
            child.sendline(f'/bin/native-framebuffer-test {nonce}; printf "\\nFRAMEBUFFER_EXIT:{nonce}:%s\\n" "$?"')
            failures = [r"NATIVE_FRAMEBUFFER_FAIL:[^\r\n]+", r"native dyld: [^\r\n]+", r"panic\(cpu", r"FRAMEBUFFER_EXIT:" + nonce + r":(\d+)"]
            for pattern in (r"NativeBootFramebuffer: console claimed", "NATIVE_FRAMEBUFFER_CONSOLE_CLAIMED:" + re.escape(nonce)):
                if child.expect([serial_line(pattern), *failures], timeout=60):
                    rejected = child.after
                    if "panic" in child.after:
                        try: child.expect(pexpect.EOF, timeout=2)
                        except pexpect.TIMEOUT: pass
                    raise RuntimeError("framebuffer console acquisition failed: " + rejected)
            ready = child.expect([serial_line("NATIVE_FRAMEBUFFER_READY:" + proof + r":(\d+):(\d+):(\d+)"), *failures], timeout=60)
            if ready:
                raise RuntimeError("framebuffer probe rejected: " + child.after)
            width, height, stride = map(int, child.match.groups())
            screenshot = work / "scanout.ppm"
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(10)
                connection.connect(str(qmp_path))
                with connection.makefile("rwb") as stream:
                    if "QMP" not in json.loads(stream.readline()):
                        raise RuntimeError("missing QMP greeting")
                    qmp_command(stream, "qmp_capabilities")
                    qmp_command(stream, "screendump", {"filename": str(screenshot)})
            samples = check_screenshot(screenshot, width, height)
            child.sendline("")
            # Explicit release, re-acquire and close-triggered automatic restore
            # must reach the real platform call successfully in this order.
            # Driver IOLog is available even when ARM disables PE kprintf.
            for pattern in (r"NativeBootFramebuffer: console restored", r"NativeBootFramebuffer: console claimed", r"NativeBootFramebuffer: console restored"):
                if child.expect([serial_line(pattern), *failures], timeout=30):
                    raise RuntimeError("framebuffer console restoration failed: " + child.after)
            checked = child.expect([serial_line("NATIVE_FRAMEBUFFER_OK:" + proof), *failures], timeout=30)
            if checked:
                raise RuntimeError("framebuffer cleanup failed: " + child.after)
            child.expect(r"\r*\nFRAMEBUFFER_EXIT:" + nonce + r":(\d+)\r*\n", timeout=10)
            if child.match.group(1) != "0":
                raise RuntimeError("framebuffer probe exited unsuccessfully")
            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
            result = {"architecture": args.arch, "nonce": nonce, "width": width, "height": height, "row_bytes": stride,
                      "screenshot_sha256": sha(screenshot), "samples": samples, "exit_status": 0,
                      "console_ownership_checked": True,
                      "proof": "Native mapping/read/write/unmap, root privilege/exclusive open, real console acquire/release/close restore and actual QMP scanout pixel match."}
            (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(f"PASS native framebuffer {args.arch}: {width}x{height}, real scanout at {screenshot}", flush=True)
    finally:
        if child is not None:
            child.terminate(force=True)
        seed.unlink(missing_ok=True)
        if args.arch == "x86_64":
            mutable.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
