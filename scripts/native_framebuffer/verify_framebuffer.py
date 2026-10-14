#!/usr/bin/env python3
"""Boot actual ARM Darwin, verify its shell and capture the QEMU framebuffer.

Run through //scripts/native_framebuffer:verify_arm64 so Bazel builds all inputs.
This verifies a firmware/kernel display prerequisite, not an Xorg/Xfce desktop.
The verifier never writes pixels or substitutes a host drawing for guest output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import socket
import tempfile

import pexpect

ROOT = Path(__file__).resolve().parents[2]


def qmp_command(stream, command, arguments=None):
    request = {"execute": command, "id": command}
    if arguments:
        request["arguments"] = arguments
    stream.write(json.dumps(request).encode() + b"\n")
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP closed before replying")
        reply = json.loads(line)
        if reply.get("id") != command:
            continue
        if "error" in reply:
            raise RuntimeError(f"QMP {command}: {reply['error']}")
        return reply.get("return")


def ppm_geometry(path):
    with path.open("rb") as stream:
        if stream.readline().strip() != b"P6":
            raise RuntimeError("QEMU did not produce a binary PPM screenshot")
        dimensions = stream.readline()
        while dimensions.startswith(b"#"):
            dimensions = stream.readline()
        width, height = map(int, dimensions.split())
        if stream.readline().strip() != b"255":
            raise RuntimeError("unexpected screenshot channel depth")
        pixels = stream.read()
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated QEMU screenshot")
    first = pixels[:3]
    changed = sum(pixels[i:i + 3] != first for i in range(0, len(pixels), 3))
    return width, height, changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".cache/framebuffer")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="arm64-", dir=args.output_dir)).resolve()
    kernel = ROOT / "bazel-bin/src/Kernel/xnu/12377.121.6/arm64/kernel.bin"
    loader = ROOT / "bazel-bin/bootloader/rpi4/boot_current.bin"
    rootfs = ROOT / "bazel-bin/userland/diagnostic/root.arm64.img"
    layout = json.loads(kernel.with_name("kernel_layout.json").read_text())
    if layout["architecture"] != "arm64" or layout["image_size"] != kernel.stat().st_size:
        raise RuntimeError("kernel layout does not match the ARM image")
    seed = work / "entropy.bin"
    seed.write_bytes(secrets.token_bytes(256))
    seed.chmod(0o600)
    qmp_path = work / "qmp.sock"
    options = ["-M", "raspi4b", "-cpu", "cortex-a72", "-m", "2G",
               "-serial", "stdio", "-monitor", "none", "-display", "none",
               "-no-reboot", "-qmp", f"unix:{qmp_path},server=on,wait=off"]
    for path, address, cpu in [(loader, 0x40080000, 0),
                               (kernel, layout["physical_base"], None),
                               (rootfs, 0x0a000000, None),
                               (seed, 0x07f00000, None)]:
        escaped = str(path.resolve()).replace(",", ",,")
        option = f"loader,file={escaped},addr={address:#x}"
        if cpu is not None:
            option += f",cpu-num={cpu}"
        options += ["-device", option]
    for cpu in range(1, 4):
        options += ["-device", f"loader,addr=0x40080000,cpu-num={cpu}"]
    serial = work / "serial.log"
    screenshot = work / "framebuffer.ppm"
    png = work / "framebuffer.png"
    print(f"Framebuffer verification artifacts: {work}", flush=True)
    child = None
    connection = None
    stream = None
    try:
        with serial.open("w") as log:
            child = pexpect.spawn("qemu-system-aarch64", options, encoding="utf-8",
                                  codec_errors="replace", echo=False, timeout=args.timeout)
            child.logfile_read = log
            child.expect(r"Framebuffer: base=0x([0-9a-f]+) width=0x([0-9a-f]+) "
                         r"height=0x([0-9a-f]+) pitch=0x([0-9a-f]+)")
            base, width, height, pitch = [int(child.match.group(i), 16) for i in range(1, 5)]
            child.expect(r"VC reserved: base=0x([0-9a-f]+) size=0x([0-9a-f]+) "
                         r"managed RAM=0x([0-9a-f]+)")
            vc_base, vc_size, managed = [int(child.match.group(i), 16) for i in range(1, 4)]
            if not (managed <= vc_base <= base < base + pitch * height <= vc_base + vc_size):
                raise RuntimeError("reported framebuffer overlaps managed RAM or exceeds VC memory")
            child.expect(r"Darwin Kernel Version 25\.5\.0:")
            child.expect(r"BCM2711Platform: published framebuffer nub, " +
                         f"{width}x{height}")
            child.expect(r"\r*\nroot[^\r\n]*# ")
            nonce = secrets.token_hex(24)
            child.sendline(f'printf "\\n{nonce}:%s:%s\\n" "$(uname -r)" "$(uname -m)"')
            child.expect(r"\r*\n" + nonce + r":25\.5\.0:arm64\r*\n", timeout=30)
            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)

            connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            connection.settimeout(10)
            connection.connect(str(qmp_path))
            stream = connection.makefile("rwb")
            greeting = json.loads(stream.readline())
            if "QMP" not in greeting:
                raise RuntimeError("invalid QMP greeting")
            qmp_command(stream, "qmp_capabilities")
            qmp_command(stream, "screendump", {"filename": str(screenshot)})
            screen_width, screen_height, changed = ppm_geometry(screenshot)
            if (screen_width, screen_height) != (width, height):
                raise RuntimeError("QEMU scanout geometry differs from mailbox framebuffer")
            if changed == 0:
                raise RuntimeError("kernel framebuffer remained a solid color")
            qmp_command(stream, "screendump", {"filename": str(png), "format": "png"})
            result = {
                "stage": "native firmware and kernel framebuffer; not Xfce",
                "architecture": "arm64", "darwin": "25.5.0", "shell_verified": True,
                "framebuffer_nub_published": True,
                "framebuffer": {"base": base, "width": width, "height": height, "pitch": pitch},
                "vc_memory": {"base": vc_base, "size": vc_size},
                "managed_memory_size": managed,
                "pixels_different_from_first": changed,
                "screenshot_sha256": hashlib.sha256(png.read_bytes()).hexdigest(),
                "input_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in (kernel, loader, rootfs)},
                "qemu_version": greeting["QMP"]["version"],
                "artifacts": {"serial": str(serial), "screenshot": str(png),
                              "raw_screenshot": str(screenshot)},
            }
            (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result, indent=2), flush=True)
            return 0
    except (OSError, RuntimeError, pexpect.TIMEOUT, pexpect.EOF) as error:
        print(f"Framebuffer verification failed: {error}; see {serial}", flush=True)
        return 1
    finally:
        if stream is not None:
            stream.close()
        if connection is not None:
            connection.close()
        if child is not None:
            child.terminate(force=True)
        seed.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
