#!/usr/bin/env python3
"""Boot native Xorg, capture its real X11 window and inject hardware input."""
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

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/"userland/native/framebuffer"))
from verify import qmp_command, qpath, serial_line, sha

def check_window(path,width,height):
    data=path.read_bytes();header=re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s",data)
    if not header or tuple(map(int,header.groups()))!=(width,height):raise RuntimeError("wrong native scanout geometry")
    pixels=data[header.end():]
    if len(pixels)!=width*height*3:raise RuntimeError("truncated actual scanout")
    samples=[];colors=[[255,0,0],[0,255,0],[0,0,255],[255,255,255]]
    left,top,w,h=width//4,height//4,width//2,height//2
    for stripe,color in enumerate(colors):
        x=left+(2*stripe+1)*w//8
        for y in (top+h//4,top+h//2,top+3*h//4):
            actual=list(pixels[(y*width+x)*3:(y*width+x)*3+3])
            if actual!=color:raise RuntimeError(f"Xorg actual scanout pixel {(x,y)} {actual} != {color}")
            samples.append({"x":x,"y":y,"rgb":actual})
    return samples

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch",choices=("arm64","x86_64"),required=True)
    parser.add_argument("--timeout",type=float,default=600)
    parser.add_argument("--output-dir",type=Path,default=ROOT/".cache/qemu-xorg")
    args=parser.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix=args.arch+"-",dir=args.output_dir)).resolve()
    package=ROOT/"bazel-bin/userland/apps/x11"
    sources={"root":package/f"xorg-root.{args.arch}.img","root_report":package/f"xorg-root.{args.arch}.json",
             "runtime_report":package/f"xorg-runtime.{args.arch}.json","collection":package/f"desktop-kernel.collection.{args.arch}"}
    if args.arch=="arm64":sources.update(kernel=package/"desktop-arm64/kernel.bin",layout=package/"desktop-arm64/kernel_layout.json",loader=package/"desktop-arm64/boot_current.bin")
    else:sources.update(boot=package/"xorg-boot.x86_64.img",firmware=ROOT/"bazel-bin/bootloader/ovmf/OVMF_CODE_4M.fd",variables=ROOT/"bazel-bin/bootloader/ovmf/OVMF_VARS-1024x768.fd")
    inputs={};records={}
    for name,source in sources.items():
        target=work/(name+source.suffix);shutil.copyfile(source,target);target.chmod(0o444)
        inputs[name]=target;records[name]={"source":str(source.resolve()),"retained":str(target),"sha256":sha(target)}
    if json.loads(inputs["root_report"].read_text())["root_sha256"]!=sha(inputs["root"]):raise RuntimeError("guest root/report mismatch")
    manifest={"architecture":args.arch,"captured_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),"inputs":records,
              "scope":"Native Xorg window rendering and actual hardware input; Xfce remains unfinished."}
    (work/"input-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    seed=work/"entropy.bin";seed.write_bytes(secrets.token_bytes(256));seed.chmod(0o600);mutable=None
    if args.arch=="arm64":
        layout=json.loads(inputs["layout"].read_text());command="qemu-system-aarch64"
        if layout["architecture"]!="arm64" or layout["image_size"]!=inputs["kernel"].stat().st_size:raise RuntimeError("invalid ARM kernel layout")
        options=["-M","raspi4b","-cpu","cortex-a72","-m","2G","-device",f"loader,file={qpath(inputs['loader'])},addr=0x40080000,cpu-num=0"]
        for cpu in (1,2,3):options += ["-device",f"loader,addr=0x40080000,cpu-num={cpu}"]
        options += ["-device",f"loader,file={qpath(inputs['kernel'])},addr={layout['physical_base']:#x}","-device",f"loader,file={qpath(seed)},addr=0x07f00000",
                    "-device",f"loader,file={qpath(inputs['root'])},addr=0x0a000000","-device","usb-hub,id=native-hub","-device","usb-kbd,port=1.1","-device","usb-mouse,port=1.2"]
    else:
        mutable=work/"boot-private.img";shutil.copyfile(inputs["boot"],mutable);mutable.chmod(0o600)
        subprocess.run(["mcopy","-o","-i",str(mutable)+"@@1048576",str(seed),"::EFI/BOOT/entropy.bin"],check=True)
        variables=work/"variables-private.fd";shutil.copyfile(inputs["variables"],variables);variables.chmod(0o600)
        command="qemu-system-x86_64"
        options=["-m","4G","-machine","q35,accel=tcg","-cpu","Penryn,vendor=GenuineIntel,vmware-cpuid-freq=on",
                 "-drive","if=pflash,format=raw,readonly=on,file="+qpath(inputs["firmware"]),"-drive","if=pflash,format=raw,file="+qpath(variables),
                 "-smbios","type=2","-device","virtio-vga","-drive","file="+qpath(mutable)+",format=raw,snapshot=on"]
    qmp=work/"qmp.sock";options += ["-serial","stdio","-monitor","none","-display","none","-no-reboot","-qmp","unix:"+qpath(qmp)+",server=on,wait=off"]
    nonce=secrets.token_hex(24);proof=re.escape(nonce+":25.5.0:"+args.arch)
    failures=[r"NATIVE_XORG_FAIL:[^\r\n]+",r"NATIVE_XORG_SERVER_EXIT:[^\r\n]+",r"native dyld: [^\r\n]+",r"panic\(cpu",r"XORG_EXIT:"+nonce+r":(\d+)"]
    child=None;print("Xorg proof directory: "+str(work),flush=True)
    try:
        with (work/"serial.log").open("w") as log:
            child=pexpect.spawn(command,options,encoding="utf-8",codec_errors="replace",echo=False,timeout=args.timeout);child.logfile_read=log
            child.expect(r"Darwin Kernel Version ([0-9.]+):")
            if child.match.group(1)!="25.5.0":raise RuntimeError("wrong Darwin guest")
            child.expect(r"\r*\nroot[^\r\n]*# ")
            child.sendline(f'/bin/native-xorg-test {nonce}; printf "\\nXORG_EXIT:{nonce}:%s\\n" "$?"')
            ready="NATIVE_XORG_READY:"+proof+r":(\d+):(\d+):(\d+):(\d+):(\d+):(-?\d+):(-?\d+)"
            if child.expect([serial_line(ready),*failures],timeout=120):raise RuntimeError("native Xorg startup failed: "+child.after)
            width,height,window,server,client,rx,ry=map(int,child.match.groups())
            if server<=1 or client<=1 or server==client or not window:raise RuntimeError("invalid actual Xorg/client process identity")
            screenshot=work/"scanout.ppm"
            with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
                connection.settimeout(10);connection.connect(str(qmp))
                with connection.makefile("rwb") as stream:
                    if "QMP" not in json.loads(stream.readline()):raise RuntimeError("missing QMP greeting")
                    qmp_command(stream,"qmp_capabilities");qmp_command(stream,"screendump",{"filename":str(screenshot)})
                    samples=check_window(screenshot,width,height);child.sendline("")
                    qmp_command(stream,"input-send-event",{"events":[{"type":"key","data":{"down":True,"key":{"type":"qcode","data":"a"}}}]})
                    if child.expect([serial_line("NATIVE_XORG_KEY:"+re.escape(nonce)+":12:97"),*failures],timeout=90):raise RuntimeError("native Xorg keyboard failed: "+child.after)
                    qmp_command(stream,"input-send-event",{"events":[{"type":"key","data":{"down":False,"key":{"type":"qcode","data":"a"}}}]})
                    qmp_command(stream,"input-send-event",{"events":[{"type":"rel","data":{"axis":"x","value":width//2-rx}},{"type":"rel","data":{"axis":"y","value":height//2-ry}}]})
                    qmp_command(stream,"input-send-event",{"events":[{"type":"btn","data":{"down":True,"button":"left"}}]})
                    if child.expect([serial_line("NATIVE_XORG_BUTTON:"+re.escape(nonce)+r":1:\d+:\d+"),*failures],timeout=90):raise RuntimeError("native Xorg pointer failed: "+child.after)
                    qmp_command(stream,"input-send-event",{"events":[{"type":"btn","data":{"down":False,"button":"left"}}]})
            for pattern in ("NATIVE_XORG_OK:"+proof,):
                if child.expect([serial_line(pattern),*failures],timeout=90):raise RuntimeError("native Xorg input/cleanup failed: "+child.after)
            child.expect(serial_line("XORG_EXIT:"+re.escape(nonce)+r":(\d+)"),timeout=10)
            if child.match.group(1)!="0":raise RuntimeError("native Xorg probe failed")
            child.expect(r"\r*\nroot[^\r\n]*# ",timeout=10)
            result={"architecture":args.arch,"nonce":nonce,"width":width,"height":height,"window":window,"server_pid":server,"client_pid":client,
                    "samples":samples,"screenshot_sha256":sha(screenshot),"keyboard_keycode":12,"keyboard_keysym":97,"button":1,"exit_status":0,"desktop_checked":False,
                    "proof":"Real native Xorg/X11 client processes, server pixel readback and QMP scanout, native physical keyboard/mouse events and clean server exit."}
            (work/"result.json").write_text(json.dumps(result,indent=2)+"\n")
            print(f"PASS native Xorg {args.arch}: real X11 window, 12 scanout samples and hardware keyboard/button input",flush=True)
    finally:
        if child is not None:child.terminate(force=True)
        seed.unlink(missing_ok=True)
        if mutable:mutable.unlink(missing_ok=True)

if __name__=="__main__":main()
