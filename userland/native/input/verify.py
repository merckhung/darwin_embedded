#!/usr/bin/env python3
"""Verify native i8042/DWC2 input from real QMP hardware events in Darwin guests."""
import argparse
import datetime
import json
from pathlib import Path
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile

import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "userland/native/framebuffer"))
from verify import qmp_command, qpath, serial_line, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), default="x86_64")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".cache/qemu-input")
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=args.arch + "-", dir=args.output_dir)).resolve()
    package = ROOT / "bazel-bin/userland/native/input"
    sources = {"root": package / f"root.{args.arch}.img", "root_report": package / f"root.{args.arch}.json",
               "driver_report": package / f"driver.{args.arch}.json", "driver": package / f"NativeBootInput.{args.arch}.tar",
               "collection": package / f"kernel.collection.{args.arch}"}
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
    if json.loads(inputs["root_report"].read_text())["root_sha256"] != sha(inputs["root"]):
        raise RuntimeError("root report does not match retained guest root")
    if json.loads(inputs["driver_report"].read_text())["archive_sha256"] != sha(inputs["driver"]):
        raise RuntimeError("driver report does not match retained driver")
    manifest = {"architecture": args.arch, "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "inputs": records, "scope": "Real native keyboard/mouse IOUserClient; not an Xfce desktop."}
    (work / "input-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    seed = work / "entropy.bin"
    seed.write_bytes(secrets.token_bytes(256))
    seed.chmod(0o600)
    mutable = None
    if args.arch == "arm64":
        layout = json.loads(inputs["layout"].read_text())
        if layout["architecture"] != "arm64" or layout["image_size"] != inputs["kernel"].stat().st_size:
            raise RuntimeError("invalid native ARM kernel layout")
        command = "qemu-system-aarch64"
        options = ["-M", "raspi4b", "-cpu", "cortex-a72", "-m", "2G",
                   "-device", f"loader,file={qpath(inputs['loader'])},addr=0x40080000,cpu-num=0"]
        for cpu in (1, 2, 3): options += ["-device", f"loader,addr=0x40080000,cpu-num={cpu}"]
        options += ["-device", f"loader,file={qpath(inputs['kernel'])},addr={layout['physical_base']:#x}",
                    "-device", f"loader,file={qpath(seed)},addr=0x07f00000",
                    "-device", f"loader,file={qpath(inputs['root'])},addr=0x0a000000",
                    "-device", "usb-hub,id=native-hub", "-device", "usb-kbd,port=1.1", "-device", "usb-mouse,port=1.2"]
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
    hardware = [
        [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "a"}}}],
        [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "a"}}}],
        [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "left"}}}],
        [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "left"}}}],
        [{"type": "rel", "data": {"axis": "x", "value": 37}}, {"type": "rel", "data": {"axis": "y", "value": 13}}],
        [{"type": "btn", "data": {"down": True, "button": "left"}}],
        [{"type": "btn", "data": {"down": False, "button": "left"}}],
        [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}}],
        [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "a"}}}],
        [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "a"}}}],
        [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}}],
    ]
    expected = [(1,4,1,0), (1,4,0,0), (1,80,1,0), (1,80,0,0), (2,0,37,13), (2,1,0,0), (2,0,0,0),
                (1,225,1,0), (1,4,1,0), (1,4,0,0), (1,225,0,0)]
    nonce = secrets.token_hex(24)
    proof = re.escape(nonce + ":25.5.0:" + args.arch)
    failures = [r"NATIVE_INPUT_FAIL:[^\r\n]+", r"native dyld: [^\r\n]+", r"panic\(cpu", r"INPUT_EXIT:" + nonce + r":(\d+)"]
    child = None
    events = []
    print("Input proof directory: " + str(work), flush=True)
    try:
        with (work / "serial.log").open("w") as log:
            child = pexpect.spawn(command, options, encoding="utf-8", codec_errors="replace", echo=False, timeout=args.timeout)
            child.logfile_read = log
            child.expect(r"Darwin Kernel Version ([0-9.]+):")
            if child.match.group(1) != "25.5.0": raise RuntimeError("incorrect guest Darwin version")
            child.expect(r"\r*\nroot[^\r\n]*# ")
            child.sendline(f'printf "\\n{nonce}:%s:%s\\n" "$(uname -r)" "$(uname -m)"')
            child.expect(serial_line(proof), timeout=60)
            child.expect(r"\r*\nroot[^\r\n]*# ")
            child.sendline(f'/bin/native-input-test {nonce}; printf "\\nINPUT_EXIT:{nonce}:%s\\n" "$?"')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(10)
                connection.connect(str(qmp_path))
                with connection.makefile("rwb") as stream:
                    if "QMP" not in json.loads(stream.readline()): raise RuntimeError("missing QMP greeting")
                    qmp_command(stream, "qmp_capabilities")
                    for stage, injection in enumerate(hardware):
                        if child.expect([serial_line("NATIVE_INPUT_READY:" + proof + ":" + str(stage)), *failures], timeout=30):
                            raise RuntimeError("native input readiness rejected: " + child.after)
                        qmp_command(stream, "input-send-event", {"events": injection})
                        event = "NATIVE_INPUT_EVENT:" + re.escape(nonce) + r":(\d+):(\d+):(\d+):(-?\d+):(-?\d+)"
                        if child.expect([serial_line(event), *failures], timeout=30):
                            raise RuntimeError("native hardware event rejected: " + child.after)
                        actual = tuple(map(int, child.match.groups()))
                        if actual[1:] != expected[stage] or actual[0] <= (events[-1]["sequence"] if events else 0):
                            raise RuntimeError("native hardware event differs from injected input")
                        events.append({"sequence": actual[0], "type": actual[1], "code": actual[2],
                                       "value": actual[3], "value2": actual[4], "qmp": injection})
            if child.expect([serial_line("NATIVE_INPUT_OK:" + proof + ":events=" + str(len(hardware))), *failures], timeout=30):
                raise RuntimeError("native input cleanup failed: " + child.after)
            child.expect(serial_line("INPUT_EXIT:" + re.escape(nonce) + r":(\d+)"), timeout=10)
            if child.match.group(1) != "0": raise RuntimeError("native input probe exited unsuccessfully")
            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
            result = {"architecture": args.arch, "nonce": nonce, "darwin": "25.5.0", "exit_status": 0,
                      "events": events, "privilege_and_exclusive_open_checked": True, "invalid_payloads_checked": True,
                      "reopen_checked": True, "arm_input_checked": args.arch == "arm64", "desktop_checked": False,
                      "proof": "QMP hardware events -> native " + ("DWC2 USB DMA/HID" if args.arch == "arm64" else "i8042 port reads") +
                               " -> privileged IOUserClient -> signed Mach-O probe."}
            (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
            print("PASS native " + args.arch + " input: " + str(len(events)) + " actual keyboard/mouse events, privilege, exclusive open, payload rejection and reopen", flush=True)
    finally:
        if child is not None: child.terminate(force=True)
        seed.unlink(missing_ok=True)
        if mutable: mutable.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
