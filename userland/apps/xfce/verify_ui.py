#!/usr/bin/env python3
"""Capture an actual native Xfce GTK dialog and exercise hardware key/button events."""
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
import time
import pexpect

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'scripts'))
sys.path.insert(0, str(ROOT / "userland/native/framebuffer"))
from verify import qmp_command, qpath, serial_line, sha

def interactive_console(child, log):
    # expect() logs decoded text; interact() forwards raw bytes even when the
    # child has an encoding. Keep the same serial log across that handoff.
    log.flush()
    child.logfile_read = log.buffer
    try:
        child.interact(escape_character=chr(29))
    finally:
        child.logfile_read = log

def check_area(path, width, height, left, top, area_width, area_height):
    data = path.read_bytes(); header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header or tuple(map(int, header.groups())) != (width, height):
        raise RuntimeError("wrong native scanout geometry")
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated native scanout")
    if min(left, top) < 0 or min(area_width, area_height) < 4 or left + area_width > width or top + area_height > height:
        raise RuntimeError("Xfce drawing area is outside the actual scanout")
    samples = []
    for stripe, color in enumerate(([255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255])):
        x = left + (2 * stripe + 1) * area_width // 8
        for y in (top + area_height // 4, top + area_height // 2, top + 3 * area_height // 4):
            actual = list(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3])
            if actual != color:
                raise RuntimeError(f"Xfce Cairo scanout pixel {(x, y)} {actual} != {color}")
            samples.append({"x": x, "y": y, "rgb": actual})
    return samples

def terminal_proof(child, qmp, nonce, proof, desktop, work, records, clock, failures, product=False):
    if child.expect([serial_line("NATIVE_XFCE_TERMINAL_READY:" + nonce + r":(\d+):(\d+):(\d+):(\d+)"), *failures], timeout=180):
        raise RuntimeError("original terminal failed to map: " + child.after)
    terminal_pid, window, width, height = map(int, child.match.groups())
    if terminal_pid <= 1 or not window:
        raise RuntimeError("invalid original terminal identity")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(10); connection.connect(str(qmp))
        with connection.makefile("rwb") as stream:
            if "QMP" not in json.loads(stream.readline()):
                raise RuntimeError("missing QMP greeting")
            qmp_command(stream, "qmp_capabilities")
            def type_keys(text):
                codes = {"/": "slash", "-": "minus", " ": "spc", "\n": "ret"}
                for character in text:
                    code = codes.get(character, character)
                    qmp_command(stream, "input-send-event", {"events": [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": code}}}]})
                    time.sleep(0.025)
                    qmp_command(stream, "input-send-event", {"events": [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": code}}}]})
                    time.sleep(0.025)
            # The original terminal executes real interactive dash. Inject this
            # command through the emulated hardware keyboard, then its PTY.
            time.sleep(2)
            type_keys(f"/bin/native-xfce-desktop-test {nonce} --pty-child\n")
            if child.expect([serial_line("NATIVE_XFCE_TERMINAL_PTY:" + nonce + r":(\d+):(\d+):80:24:(/dev/ttys[^\r\n]+)"), *failures], timeout=180):
                raise RuntimeError("original terminal did not execute the hardware-typed shell command: " + child.after)
            pty_pid, session_pid = map(int, child.match.groups()[:2]); tty = child.match.group(3)
            if min(pty_pid, session_pid) <= 1 or pty_pid == terminal_pid:
                raise RuntimeError("invalid real PTY process/session")
            type_keys(nonce + "\n")
            if child.expect([serial_line("NATIVE_XFCE_TERMINAL_INPUT_OK:" + nonce), *failures], timeout=120):
                raise RuntimeError("original terminal PTY challenge failed: " + child.after)
            time.sleep(2)
            screenshot = work / "scanout.ppm"
            qmp_command(stream, "screendump", {"filename": str(screenshot)})
            if not product:
                type_keys("exit\n")
    from PIL import Image
    image = Image.open(screenshot)
    if image.size != (width, height):
        raise RuntimeError("wrong actual terminal scanout geometry")
    image.save(work / "scanout.png")
    child.sendline(nonce)
    if child.expect([serial_line("NATIVE_XFCE_UI_OK:" + proof), *failures], timeout=120):
        raise RuntimeError("original terminal/session cleanup failed: " + child.after)
    child.expect(serial_line("XFCE_UI_EXIT:" + nonce + r":(\d+)"), timeout=20)
    if child.match.group(1) != "0":
        raise RuntimeError("native terminal probe failed")
    child.expect(r"\r*\nroot[^\r\n]*# ", timeout=20)
    (work / "result.json").write_text(json.dumps({"architecture": proof.split(":")[-1], "nonce": nonce,
        "terminal_pid": terminal_pid, "terminal_window": window, "pty_child_pid": pty_pid, "pty_session_pid": session_pid,
        "tty": tty, "columns": 80, "rows": 24, "width": width, "height": height, "desktop_components": desktop,
        "monotonic_clock": clock, "screenshot_sha256": sha(screenshot), "exit_status": 0,
        "hardware_keyboard_to_interactive_shell": True, "hardware_keyboard_to_pty_child": True,
        "desktop_checked": product, "automatic_desktop_startup": product, "proof": "Original xfce4-terminal and VTE on the native Xfce session; actual interactive dash executes a hardware-typed command; its foreground Darwin PTY child reads a fresh hardware-typed challenge."}, indent=2) + "\n")
    lifecycle = "automatic desktop startup" if product else "clean session exit"
    print("PASS original Xfce terminal: hardware keyboard, interactive shell, native Darwin PTY and " + lifecycle, flush=True)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--interactive", action="store_true", help="boot the product desktop and keep the serial console open; Ctrl-] exits")
    parser.add_argument('--verify-before-interactive',action='store_true',help='verify the original desktop, native tools and hardware PTY input before keeping the console open')
    parser.add_argument("--display", choices=("none", "gtk", "sdl"), default="none", help="QEMU host display backend")
    parser.add_argument("--wm", action="store_true", help="also require actual xfwm4 ownership and window reparenting")
    parser.add_argument("--desktop", action="store_true", help="require original session manager, desktop and two panels; terminal still pending")
    parser.add_argument("--product", action="store_true", help="verify the product root boots directly to the original Xfce desktop")
    parser.add_argument("--terminal", action="store_true", help="run original xfce4-terminal and verify hardware input through its real Darwin PTY")
    parser.add_argument("--jpeg", action="store_true", help="also decode the original JPEG backdrop through native GdkPixbuf")
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".cache/qemu-xfce-ui")
    args = parser.parse_args(); args.terminal = args.terminal or args.product; args.desktop = args.desktop or args.jpeg or args.terminal; args.wm = args.wm or args.desktop; args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.verify_before_interactive and not (args.interactive and args.product):
        parser.error('--verify-before-interactive requires --interactive --product')
    work = Path(tempfile.mkdtemp(prefix=args.arch + "-", dir=args.output_dir)).resolve()
    package = ROOT / "bazel-bin/userland/apps/xfce"; xorg = ROOT / "bazel-bin/userland/apps/x11"
    suite = "terminal" if args.terminal else "jpeg" if args.jpeg else "desktop" if args.desktop else "wm" if args.wm else "ui"
    sources = {"root": package / f"xfce-{suite}-root.{args.arch}.img", "root_report": package / f"xfce-{suite}-root.{args.arch}.json",
               "runtime_report": package / f"xfce-{suite}-runtime.{args.arch}.json", "collection": xorg / f"desktop-kernel.collection.{args.arch}"}
    if args.product:
        product = ROOT / "bazel-bin/rootfs"
        sources.update(root=product / f"root.{args.arch}.img", root_report=product / f"root.{args.arch}.json")
    if args.arch == "arm64":
        sources.update(kernel=xorg / "desktop-arm64/kernel.bin", layout=xorg / "desktop-arm64/kernel_layout.json", loader=xorg / "desktop-arm64/boot_current.bin")
        if args.desktop:
            sources["loader"] = package / "desktop-arm64/boot_current.bin"
    else:
        sources.update(boot=package / f"xfce-{suite}-boot.x86_64.img", firmware=ROOT / "bazel-bin/bootloader/ovmf/OVMF_CODE_4M.fd", variables=ROOT / "bazel-bin/bootloader/ovmf/OVMF_VARS-1024x768.fd")
    if args.product and args.arch == "x86_64":
        sources["boot"] = ROOT / "bazel-bin/rootfs/boot.x86_64.img"
    inputs, records = {}, {}
    for name, source in sources.items():
        target = work / (name + source.suffix); shutil.copyfile(source, target); target.chmod(0o444)
        inputs[name] = target; records[name] = {"source": str(source.resolve()), "retained": str(target), "sha256": sha(target)}
    root_report = json.loads(inputs["root_report"].read_text()); runtime_report = json.loads(inputs["runtime_report"].read_text())
    if root_report["architecture"] != args.arch or root_report["root_sha256"] != sha(inputs["root"]) or root_report["runtime_sha256"] != runtime_report["bundle_sha256"]:
        raise RuntimeError("native UI root/runtime report mismatch")
    (work / "input-manifest.json").write_text(json.dumps({"architecture": args.arch,
        "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "inputs": records,
        "scope": "Original Xfce desktop and terminal with automatic boot startup." if args.product else "Native original Xfce components and hardware input; product automatic startup is verified separately."}, indent=2) + "\n")
    seed = work / "entropy.bin"; seed.write_bytes(secrets.token_bytes(256)); seed.chmod(0o600); mutable = None
    if args.arch == "arm64":
        layout = json.loads(inputs["layout"].read_text()); command = "qemu-system-aarch64"
        if layout["architecture"] != args.arch or layout["image_size"] != inputs["kernel"].stat().st_size:
            raise RuntimeError("invalid native ARM kernel layout")
        options = ["-M", "raspi4b", "-cpu", "cortex-a72", "-m", "2G", "-device", f"loader,file={qpath(inputs['loader'])},addr=0x40080000,cpu-num=0"]
        # Match the firmware request before GUI refresh starts. QEMU 10.1's
        # initial resize from 640x480/16-bit can crash on small host displays.
        options += ['-global','bcm2835-fb.xres=1024','-global','bcm2835-fb.yres=768',
                    '-global','bcm2835-fb.bpp=32','-global','bcm2835-fb.pixo=0']
        for cpu in (1, 2, 3):
            options += ["-device", f"loader,addr=0x40080000,cpu-num={cpu}"]
        options += ["-device", f"loader,file={qpath(inputs['kernel'])},addr={layout['physical_base']:#x}",
                    "-device", f"loader,file={qpath(seed)},addr=0x07f00000", "-device", f"loader,file={qpath(inputs['root'])},addr=0x0a000000",
                    "-device", "usb-hub,id=native-hub", "-device", "usb-kbd,port=1.1", "-device", "usb-mouse,port=1.2"]
    else:
        mutable = work / "boot-private.img"; shutil.copyfile(inputs["boot"], mutable); mutable.chmod(0o600)
        subprocess.run(["mcopy", "-o", "-i", str(mutable) + "@@1048576", str(seed), "::EFI/BOOT/entropy.bin"], check=True)
        variables = work / "variables-private.fd"; shutil.copyfile(inputs["variables"], variables); variables.chmod(0o600)
        command = "qemu-system-x86_64"
        options = ["-m", "4G", "-machine", "q35,accel=tcg", "-cpu", "Penryn,vendor=GenuineIntel,vmware-cpuid-freq=on",
                   "-drive", "if=pflash,format=raw,readonly=on,file=" + qpath(inputs["firmware"]),
                   "-drive", "if=pflash,format=raw,file=" + qpath(variables), "-smbios", "type=2", "-device", "virtio-vga",
                   "-drive", "file=" + qpath(mutable) + ",format=raw,snapshot=on"]
    display='gtk,zoom-to-fit=on' if args.display=='gtk' else args.display
    qmp = work / "qmp.sock"; options += ["-name", "Darwin Xfce4 " + args.arch, "-serial", "stdio", "-monitor", "none", "-display", display, "-no-reboot", "-qmp", "unix:" + qpath(qmp) + ",server=on,wait=off"]
    nonce = secrets.token_hex(24); proof = re.escape(nonce + ":25.5.0:" + args.arch)
    failures = [r"NATIVE_XFCE_UI_FAIL:[^\r\n]+", r"native dyld: [^\r\n]+", r"received an X Window System error", r"panic\(cpu", r"XFCE_UI_EXIT:" + nonce + r":(\d+)"]
    child = None; print("Xfce UI proof directory: " + str(work), flush=True)
    try:
        with (work / "serial.log").open("w") as log:
            child = pexpect.spawn(command, options, encoding="utf-8", codec_errors="replace", echo=False, timeout=args.timeout); child.logfile_read = log
            child.expect(r"Darwin Kernel Version ([0-9.]+):")
            if child.match.group(1) != "25.5.0":
                raise RuntimeError("wrong Darwin guest")
            child.expect(r"\r*\nroot[^\r\n]*# ")
            if args.product and (not args.interactive or args.verify_before_interactive):
                from verify_native_tools import verify_tools
                verify_tools(child,work,args.arch,nonce)
            if args.interactive and not args.verify_before_interactive:
                if not args.product:
                    raise RuntimeError("interactive desktop boot requires the product root")
                print("Native Xfce starts automatically; Ctrl-] exits the serial console.", flush=True)
                interactive_console(child, log)
                return
            clock = None
            if args.wm:
                probe_name = "native-xfce-desktop-test" if args.desktop else "native-xfce-ui-test"
                child.sendline(f'/bin/{probe_name} {nonce} --clock; printf "\\nXFCE_CLOCK_EXIT:{nonce}:%s\\n" "$?"')
                if child.expect([serial_line("NATIVE_XFCE_CLOCK_READY:" + nonce), *failures], timeout=60):
                    raise RuntimeError("native clock probe failed: " + child.after)
                clock_start = time.monotonic_ns(); time.sleep(5)
                child.sendline(nonce)
                if child.expect([serial_line("NATIVE_XFCE_CLOCK_ELAPSED:" + nonce + r":(\d+)"), *failures], timeout=20):
                    raise RuntimeError("native clock reply failed: " + child.after)
                guest_elapsed = int(child.match.group(1)); host_elapsed = time.monotonic_ns() - clock_start
                ratio = guest_elapsed / host_elapsed
                if not 0.85 <= ratio <= 1.15:
                    raise RuntimeError(f"native clock disagrees with host elapsed time: ratio {ratio:.3f}")
                child.expect(serial_line("XFCE_CLOCK_EXIT:" + nonce + r":(\d+)"), timeout=20)
                if child.match.group(1) != "0":
                    raise RuntimeError("native clock probe failed")
                child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
                clock = {"host_elapsed_ns": host_elapsed, "guest_elapsed_ns": guest_elapsed, "guest_host_ratio": ratio}
                print(f"PASS native clock {args.arch}: guest/host elapsed ratio {ratio:.4f}", flush=True)
            mode = " --terminal" if args.terminal else " --jpeg" if args.jpeg else " --desktop" if args.desktop else " --wm" if args.wm else ""
            probe_name = "native-xfce-desktop-test" if args.desktop else "native-xfce-ui-test"
            launch = f'/bin/{probe_name} {nonce} --product' if args.product else f'/usr/bin/dbus-run-session -- /bin/{probe_name} {nonce}{mode}'
            child.sendline(launch + f'; printf "\\nXFCE_UI_EXIT:{nonce}:%s\\n" "$?"')
            jpeg = None
            if args.jpeg:
                if child.expect([serial_line("NATIVE_XFCE_JPEG:" + nonce + r":(\d+):(\d+)" + r":(\d+)" * 9), *failures], timeout=90):
                    raise RuntimeError("native original JPEG decoding failed: " + child.after)
                decoded = list(map(int, child.match.groups()))
                expected = [2880, 1800, 75, 144, 177, 0, 0, 0, 31, 120, 164]
                if decoded[:2] != expected[:2] or any(abs(a-b)>3 for a,b in zip(decoded[2:],expected[2:])):
                    raise RuntimeError("native JPEG dimensions/pixels differ from the pinned original backdrop: " + repr(decoded))
                jpeg = {"path": "/usr/share/backgrounds/xfce/xfce-blue.jpg", "width": decoded[0], "height": decoded[1], "rgb_samples": [decoded[2:5], decoded[5:8], decoded[8:11]]}
            daemon_pid = daemon_parent = daemon_session = None
            if not args.product:
                if child.expect([serial_line("NATIVE_XFCE_UI_DAEMON:" + nonce + r":(\d+):(\d+):(\d+)"), *failures], timeout=60):
                    raise RuntimeError("native bootstrap/daemon check failed: " + child.after)
                daemon_pid, daemon_parent, daemon_session = map(int, child.match.groups())
                if daemon_pid <= 1 or daemon_parent != 1 or daemon_session != daemon_pid:
                    raise RuntimeError("invalid actual daemon process, parent or session")
            manager_pid = manager_window = frame = None
            desktop = None
            if args.desktop:
                if child.expect([serial_line("NATIVE_XFCE_DESKTOP_READY:" + nonce + r":(\d+):(\d+):(\d+):(\d+):(\d+):(\d+):(\d+)"), *failures], timeout=420):
                    raise RuntimeError("actual Xfce desktop/session/panels failed: " + child.after)
                session, desktop_pid, panel_pid, desktop_window, top, bottom, clients = map(int, child.match.groups())
                if min(session, desktop_pid, panel_pid) <= 1 or len({session, desktop_pid, panel_pid}) != 3 or min(desktop_window, top, bottom) < 1 or len({desktop_window, top, bottom}) != 3 or clients < 3:
                    raise RuntimeError("invalid actual desktop/session/panel identities")
                desktop = {"session_pid": session, "desktop_pid": desktop_pid, "panel_pid": panel_pid, "desktop_window": desktop_window, "panel_windows": [top, bottom], "registered_session_clients": clients}
            if args.terminal:
                if args.product:
                    if child.expect([serial_line("NATIVE_XFCE_PRODUCT_CLIENTS:" + nonce + r":(\d+):(\d+)"), *failures], timeout=120):
                        raise RuntimeError("automatic original window manager/settings daemon failed: " + child.after)
                    desktop["xfwm4_pid"], desktop["xfsettingsd_pid"] = map(int, child.match.groups())
                terminal_proof(child, qmp, nonce, nonce + ":25.5.0:" + args.arch, desktop, work, records, clock, failures, args.product)
                if args.verify_before_interactive:
                    print('Desktop and native utilities verified; interactive console ready. Ctrl-] exits.',flush=True)
                    interactive_console(child,log)
                return
            if args.wm:
                if child.expect([serial_line("NATIVE_XFCE_WM_READY:" + nonce + r":(\d+):(\d+):(\d+)"), *failures], timeout=420):
                    raise RuntimeError("native xfwm4 did not manage the real GTK window: " + child.after)
                manager_pid, manager_window, frame = map(int, child.match.groups())
                if manager_pid <= 1 or not manager_window or not frame or manager_window == frame:
                    raise RuntimeError("invalid actual xfwm4 process, check window or frame")
            ready = "NATIVE_XFCE_UI_READY:" + proof + r":(\d+):(\d+):(\d+):(\d+):(\d+)" + r":(-?\d+)" * 10
            if child.expect([serial_line(ready), *failures], timeout=180):
                raise RuntimeError("native Xfce GTK startup failed: " + child.after)
            width, height, window, server, client, ax, ay, aw, ah, bx, by, rx, ry, dw, dh = map(int, child.match.groups())
            if min(server, client) <= 1 or len({server, client, daemon_pid}) != 3 or not window or min(dw, dh) < 100 or not 0 <= bx < width or not 0 <= by < height:
                raise RuntimeError("invalid actual Xfce UI process/window identity or geometry")
            if args.wm and (len({server, client, daemon_pid, manager_pid}) != 4 or len({window, manager_window, frame}) != 3):
                raise RuntimeError("xfwm4 must independently manage the real application window")
            screenshot = work / "scanout.ppm"
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(10); connection.connect(str(qmp))
                with connection.makefile("rwb") as stream:
                    if "QMP" not in json.loads(stream.readline()):
                        raise RuntimeError("missing QMP greeting")
                    qmp_command(stream, "qmp_capabilities")
                    # XSync acknowledges requests; the GTK frame clock and Xorg's
                    # framebuffer update can still be pending on a busy guest.
                    # Require the actual scanout, with a bounded presentation wait.
                    presentation_start = time.monotonic(); captures = 0
                    while True:
                        qmp_command(stream, "screendump", {"filename": str(screenshot)})
                        captures += 1
                        try:
                            samples = check_area(screenshot, width, height, ax, ay, aw, ah)
                            break
                        except RuntimeError:
                            if time.monotonic() - presentation_start >= 60:
                                raise
                            time.sleep(0.25)
                    presentation = {"captures": captures, "elapsed_seconds": time.monotonic() - presentation_start}
                    qmp_command(stream, "input-send-event", {"events": [{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "a"}}}]})
                    if child.expect([serial_line("NATIVE_XFCE_UI_KEY:" + nonce + ":12:97"), *failures], timeout=90):
                        raise RuntimeError("native GTK keyboard failed: " + child.after)
                    qmp_command(stream, "input-send-event", {"events": [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "a"}}}]})
                    qmp_command(stream, "input-send-event", {"events": [{"type": "rel", "data": {"axis": "x", "value": bx - rx}}, {"type": "rel", "data": {"axis": "y", "value": by - ry}}]})
                    if child.expect([serial_line(f"NATIVE_XFCE_UI_POINTER:{nonce}:{bx}:{by}"), *failures], timeout=60):
                        raise RuntimeError("native pointer did not reach the real Xfce response button: " + child.after)
                    qmp_command(stream, "input-send-event", {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]})
                    if child.expect([serial_line("NATIVE_XFCE_UI_BUTTON:" + nonce + ":1"), *failures], timeout=60):
                        raise RuntimeError("native GTK button press failed: " + child.after)
                    qmp_command(stream, "input-send-event", {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]})
            for pattern in ("NATIVE_XFCE_UI_RESPONSE:" + nonce + ":-5", "NATIVE_XFCE_UI_OK:" + proof):
                if child.expect([serial_line(pattern), *failures], timeout=90):
                    raise RuntimeError("native Xfce response/cleanup failed: " + child.after)
            child.expect(serial_line("XFCE_UI_EXIT:" + nonce + r":(\d+)"), timeout=20)
            if child.match.group(1) != "0":
                raise RuntimeError("native Xfce UI probe failed")
            child.expect(r"\r*\nroot[^\r\n]*# ", timeout=10)
            (work / "result.json").write_text(json.dumps({"architecture": args.arch, "nonce": nonce, "width": width, "height": height,
                "window": window, "server_pid": server, "client_pid": client, "drawing_area": [ax, ay, aw, ah], "button_position": [bx, by],
                "daemon_pid": daemon_pid, "daemon_parent_pid": daemon_parent, "daemon_session": daemon_session,
                "xfwm4_pid": manager_pid, "xfwm4_check_window": manager_window, "xfwm4_frame_window": frame,
                "monotonic_clock": clock,
                "scanout_presentation": presentation,
                "jpeg_decode": jpeg,
                "desktop_components": desktop,
                "bootstrap_rpc_invalid_destination_checked": True, "text_unknown_glyphs": 0,
                "samples": samples, "screenshot_sha256": sha(screenshot), "keyboard_keycode": 12, "keyboard_keysym": 97,
                "button": 1, "dialog_response": -5, "exit_status": 0, "desktop_checked": False,
                "proof": "Original Xfce titled dialog on native GTK/X11; real Cairo scanout and hardware input produce an actual GTK response."}, indent=2) + "\n")
            print(f"PASS native Xfce {suite} {args.arch}: actual GTK dialog, 12 scanout samples, hardware key and button response", flush=True)
    finally:
        if child is not None:
            child.terminate(force=True)
        seed.unlink(missing_ok=True)
        if mutable:
            mutable.unlink(missing_ok=True)

if __name__ == "__main__":
    main()
