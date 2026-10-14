#!/usr/bin/env python3
"""Compile original Xorg protocol/rendering layers for the native Darwin DDX."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gtk"))
from build_gtk import meson_list
from build_accessibility import merge_archive, run, symbols, definitions, digest, pack

VERSION = "21.1.25"
LAYERS = {"dix": "srcs_dix", "mi": "srcs_mi", "os": "srcs_os", "fb": "srcs_fb", "composite": "srcs_composite",
          "damageext": "srcs_damageext", "dbe": "srcs_dbe", "miext/damage": "srcs_miext_damage", "miext/shadow": "srcs_miext_shadow",
          "miext/sync": "srcs_miext_sync", "present": "srcs_present", "randr": "srcs_randr", "record": "srcs_record",
          "render": "srcs_render", "xfixes": "srcs_xfixes", "xkb": "srcs_xkb", "Xext": "srcs_xext", "Xi": "srcs_xi"}
OPTIONS = {"build_mitshm", "build_hashtable", "build_res", "build_screensaver", "build_xace", "build_xinerama", "build_xsecurity"}


def layer_inventory(recipe, variable):
    names = meson_list(recipe, variable)
    active = [True]
    for line in recipe.splitlines():
        if line.startswith("if "): active.append(active[-1] and line[3:].strip() in OPTIONS)
        elif line.startswith("endif"): active.pop()
        elif active[-1] and re.match(r"^\s*" + re.escape(variable) + r"\s*\+=", line):
            names += re.findall(r"'([^']+\.c)'", line)
    return [name for name in names if name.endswith(".c")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("root", "sdk", "dependencies", "output", "report"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args(); root = args.root.resolve(); source = root / "src/Applications/xserver/xorg-server" / VERSION
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-xorg-", dir=args.output.parent) as directory:
        work = Path(directory).resolve(); os.environ["TMPDIR"] = str(work)
        sdk, install, generated = work / "sdk", work / "install", work / "generated"; generated.mkdir()
        with tarfile.open(args.sdk) as archive: archive.extractall(sdk, filter="data")
        merge_archive(args.dependencies, install)
        if json.loads((sdk / "DarwinSDK.json").read_text())["architecture"] != args.arch: raise ValueError("Xorg SDK architecture mismatch")
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC", "-fvisibility=hidden",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-strict-aliasing", "-nostdinc", "-D_DARWIN_C_SOURCE=1", "-D__BSD_VISIBLE=1",
                 "-D_FORTIFY_SOURCE=0", "-DHAVE_DIX_CONFIG_H", "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types", "-Wno-availability",
                 "-isystem", sdk / "usr/include", "-isystem", resource + "/include", "-I", install / "usr/include",
                 "-I", install / "usr/include/pixman-1", "-I", install / "usr/include/glib-2.0", "-I", generated, "-I", source]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        include_recipe = (source / "include/meson.build").read_text()
        config = {"_DIX_CONFIG_H_": 1, "HAVE_TYPEOF": 1, "_XSERVER64": 1, "_GNU_SOURCE": 1, "_XTYPEDEF_POINTER": 1, "_XITYPEDEF_POINTER": 1,
                  "X_BYTE_ORDER": "X_LITTLE_ENDIAN", "MONOTONIC_CLOCK": 1, "TCPCONN": 1, "UNIXCONN": 1, "IPv6": 1,
                  "SERVER_MISC_CONFIG_PATH": '"/usr/share/xorg"', "PROJECTROOT": '"/usr"', "SYSCONFDIR": '"/etc"',
                  "COMPILEDDEFAULTFONTPATH": '"built-ins,/usr/share/fonts"', "XORG_VERSION_CURRENT": 12101025, "CSRG_BASED": 1,
                  "__XSERVERNAME__": '"Xorg"', "XTRANS_SEND_FDS": 1, "LISTEN_UNIX": 1, "LISTEN_TCP": 0, "LISTEN_LOCAL": 0,
                  "VENDOR_RELEASE": 12101025, "VENDOR_NAME": '"X.Org Foundation"', "VENDOR_NAME_SHORT": '"X.Org"', "VENDOR_WEB": '"https://www.x.org"',
                  "XKB_BIN_DIRECTORY": '"/usr/bin"', "XKB_BASE_DIRECTORY": '"/usr/share/X11/xkb"', "XKB_DFLT_RULES": '"base"', "XKB_DFLT_MODEL": '"pc105"',
                  "XKB_DFLT_LAYOUT": '"us"', "XKB_DFLT_VARIANT": '""', "XKB_DFLT_OPTIONS": '""', "XKM_OUTPUT_DIR": '"/tmp/xkb/"',
                  "XORG_BIN_DIRECTORY": '"/usr/bin"', "XCONFIGFILE": '"xorg.conf"', "DEFAULT_LOGDIR": '"/var/log"', "DEFAULT_LOGPREFIX": '"Xorg."',
                  "DEFAULT_LIBRARY_PATH": '"/usr/lib"', "DEFAULT_MODULE_PATH": '"/usr/lib/xorg/modules"'}
        for feature in ("BIGREQS", "COMPOSITE", "DAMAGE", "DBE", "HAS_SHM", "MITSHM", "PANORAMIX", "PRESENT", "RANDR", "RES", "RENDER",
                        "SCREENSAVER", "SHAPE", "XACE", "XCMISC", "XCSECURITY", "XFIXES", "XINERAMA", "XINPUT", "XRECORD", "XSYNC", "XTEST"):
            config[feature] = 1
        features = {}
        def probe(name, text):
            path = generated / "feature.c"; path.write_text(text)
            result = subprocess.run([*map(str, flags), "-Werror", "-c", str(path), "-o", str(generated / "feature.o")], capture_output=True)
            features[name] = result.returncode == 0
            if not result.returncode: config[name] = 1
        for key, header in re.findall(r"conf_data.set\('([^']+)', cc.has_header\('([^']+)'\)", include_recipe):
            probe(key, '#include <' + header + '>\n')
        includes = "".join('#include <' + name + '>\n' for name in ("stdio.h", "stdlib.h", "strings.h", "string.h", "unistd.h", "poll.h", "signal.h", "sys/socket.h",
                          "sys/time.h", "sys/mman.h", "time.h", "math.h", "ifaddrs.h", "netdb.h"))
        for key, function in re.findall(r"conf_data.set\('([^']+)', cc.has_function\('([^']+)'", include_recipe):
            probe(key, includes + 'void *p=(void*)&' + function + ';\n')
        for name, text in {
            "HAVE_SOCKLEN_T": '#include <sys/socket.h>\nsocklen_t value;\n',
            "HAVE_STRUCT_SOCKADDR_STORAGE": '#include <sys/socket.h>\nstruct sockaddr_storage value;\n',
            "MONOTONIC_CLOCK": '#include <time.h>\nint f(void){struct timespec t;return clock_gettime(CLOCK_MONOTONIC,&t);}\n',
            "INPUTTHREAD": '#include <pthread.h>\nint f(void){return PTHREAD_MUTEX_RECURSIVE;}\n',
        }.items(): probe(name, text)
        config["BUSFAULT"] = int(features.get("HAVE_SIGACTION", False))
        for header in ("dix-config.h", "version-config.h", "xkb-config.h", "xorg-config.h"):
            (generated / header).write_text("/* Native Darwin target probes and explicit XNU DDX configuration. */\n" + "".join(f"#define {key} {value}\n" for key, value in sorted(config.items())))
        common = (source / "meson.build").read_text()
        include_paths = re.search(r"^inc\s*=\s*include_directories\((.*?)\)", common, re.M | re.S)
        if not include_paths: raise ValueError("original Xorg include inventory missing")
        for include in re.findall(r"'([^']+)'", include_paths[1]): flags += ["-I", source / include]
        files = []
        inventories = {}
        for layer, variable in LAYERS.items():
            recipe = (source / layer / "meson.build").read_text()
            names = [name for name in layer_inventory(recipe, variable) if not (layer == "os" and name == "xsha1.c")]
            inventories[layer] = names; files += [source / layer / name for name in names]
        files += [source / "dix/stubmain.c", source / "mi/miinitext.c", root / "userland/apps/x11/xorg_sha1.c"]
        for function in ("reallocarray", "strcasecmp", "strcasestr", "strlcat", "strlcpy", "strndup", "timingsafe_memcmp"):
            if not features.get("HAVE_" + function.upper(), False): files.append(source / "os" / (function + ".c"))
        if config["BUSFAULT"]: files.append(source / "os/busfault.c")
        # Preserve Apple's sysctl-based process identity query while using
        # the already verified native pthread_once initializer. The release
        # file remains unchanged; only this private compilation copy differs.
        original_client = source / "os/client.c"
        client_text = original_client.read_text()
        pattern = r"        static dispatch_once_t once;\n        static int argmax;\n        dispatch_once\(&once, \^\{(.*?)        \}\);"
        match = re.search(pattern, client_text, re.S)
        if not match: raise ValueError("original Darwin client initializer changed")
        helper = ('\n#ifdef __APPLE__\nstatic pthread_once_t xnu_argmax_once = PTHREAD_ONCE_INIT;\nstatic int xnu_argmax;\n'
                  'static void xnu_initialize_argmax(void)\n{' + match[1].replace("argmax", "xnu_argmax") + '}\n#endif\n')
        client_text = client_text.replace('#include <dispatch/dispatch.h>', '#include <pthread.h>')
        client_text = client_text.replace('void\nDetermineClientCmd', helper + '\nvoid\nDetermineClientCmd')
        client_text = re.sub(pattern, '        pthread_once(&xnu_argmax_once, xnu_initialize_argmax);\n        int argmax = xnu_argmax;', client_text, flags=re.S)
        compiled_client = generated / "client.c"; compiled_client.write_text(client_text)
        files = [compiled_client if path == original_client else path for path in files]
        original_shm = source / "Xext/shm.c"; shm_text = original_shm.read_text()
        before = '''        if ((shmdesc->addr == ((char *) -1)) || SHMSTAT(stuff->shmid, &buf)) {
            free(shmdesc);
            return BadAccess;
        }'''
        after = '''        if ((shmdesc->addr == ((char *) -1)) || SHMSTAT(stuff->shmid, &buf)) {
            ErrorF("Native Xorg SHM: id=%u attach=%p errno=%d\\n", stuff->shmid, shmdesc->addr, errno);
            free(shmdesc);
            return BadAccess;
        }'''
        if shm_text.count(before) != 1:
            raise ValueError("original Xorg shared-memory attach check changed")
        shm_text = '#include <errno.h>\n' + shm_text.replace(before, after)
        before = '''        if (shm_access(client, &(SHM_PERM(buf)), stuff->readOnly) == -1) {
            shmdt(shmdesc->addr);'''
        after = '''        if (shm_access(client, &(SHM_PERM(buf)), stuff->readOnly) == -1) {
            ErrorF("Native Xorg SHM: id=%u actual peer credentials/access rejected\\n", stuff->shmid);
            shmdt(shmdesc->addr);'''
        if shm_text.count(before) != 1:
            raise ValueError("original Xorg shared-memory access check changed")
        compiled_shm = generated / "shm.c"; compiled_shm.write_text(shm_text.replace(before, after))
        files = [compiled_shm if path == original_shm else path for path in files]
        flags += ["-DCLIENTIDS"]
        jobs = [(path, generated / f"xorg_{index:03d}.o") for index, path in enumerate(files)]
        def compile_one(job):
            path, obj = job
            try: run([*flags, "-c", path, "-o", obj])
            except RuntimeError as error: return {"error": str(error)}
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1): raise ValueError("non-native Xorg object")
            original = original_client if path == compiled_client else original_shm if path == compiled_shm else path
            return {"source": str(original.relative_to(root)), "source_sha256": digest(original), "compiled_source_sha256": digest(path),
                    "member": obj.name, "object_sha256": digest(obj)}
        with ThreadPoolExecutor(max_workers=8) as pool: records = list(pool.map(compile_one, jobs))
        errors = [record["error"] for record in records if "error" in record]
        if errors: raise ValueError("native Xorg compilation failed:\n" + "\n".join(errors))
        archive = install / "usr/libexec/xorg/core.a"; archive.parent.mkdir(parents=True)
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *[obj for _, obj in jobs]])
        for path in generated.glob("*.h"): shutil.copyfile(path, archive.parent / path.name)
        exported, imported, weak = symbols(archive)
        unresolved = sorted(imported - definitions(archive))
        pack(install, args.output)
        args.report.write_text(json.dumps({"architecture": args.arch, "version": VERSION, "sources": records, "configuration": config,
            "sdk_features": features, "inventories": inventories, "exports": sorted(exported), "definitions": sorted(definitions(archive)), "unresolved": unresolved, "weak_imports": sorted(weak),
            "bundle_sha256": digest(args.output), "runtime_checked": False, "ddx_linked": False, "sha1_provider": "Existing genuine native GLib GChecksum SHA1"}, indent=2) + "\n")
        print(f"Native Xorg core {args.arch}: {len(records)} Mach-O objects; requires real native DDX and program linkage")


if __name__ == "__main__": main()
