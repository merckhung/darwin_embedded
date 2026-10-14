#!/usr/bin/env python3
"""Build real OpenBSD asr/files services against the native Darwin ABI."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from native_components import run, symbols

OPENBSD = "fb109c562f6c1fced0a18e5ac2791d6d41a8b2f6"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ("root", "sdk", "output", "report"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--arch", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    own = root / "userland/native/resolver"
    sources = root / "src/Applications/resolver"
    asr = sources / "opensmtpd/7.8.0p1/openbsd-compat/libasr"
    bsd = sources / "openbsd" / OPENBSD
    libraries = root / "src/Libraries/25.5.0"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".native-resolver-", dir=args.output.parent) as directory:
        work = Path(directory).resolve()
        sdk = work / "sdk"
        with tarfile.open(args.sdk) as archive:
            archive.extractall(sdk, filter="data")
        generated = work / "source"
        shutil.copytree(asr, generated)
        (generated / "include/arpa").mkdir(parents=True)
        for name in ("arpa/nameser.h", "resolv.h"):
            shutil.copyfile(bsd / "include" / name, generated / "include" / name)
        defines = (asr.parent / "defines.h").read_text()
        time_macro = defines[defines.index("#ifndef timespecsub"):defines.index("/* needed by smtpd */", defines.index("#ifndef timespecsub"))]
        (generated / "time_math.h").write_text(time_macro)
        (generated / "includes.h").write_text('#include "native_config.h"\n')
        # Replace the portable mail daemon's single-thread fallback with actual
        # pthread-key state. The default context never crosses thread boundaries.
        (generated / "thread_private.h").write_text('''
#define _THREAD_PRIVATE_DT(a,b,c,d) native_asr_state()
''')
        path = generated / "asr.c"
        text = path.read_text()
        text = text.replace("static void *__THREAD_NAME(_asr);", "static struct asr **native_asr_state(void);")
        text = text.replace("static struct asr *_asr = NULL;", "")
        text = text.replace("static int\t init = 0;", "")
        start = text.index("\tif (init == 0) {")
        end = text.index("\n\tif ((asr = calloc", start)
        text = text[:start] + text[end:]
        text = text.replace("\t\tif (*priv == NULL)", "\t\tif (priv == NULL || *priv == NULL)", 1)
        marker = "\t\tif (*priv == NULL) {"
        text = text.replace(marker, "\t\tif (priv == NULL) return NULL;\n" + marker, 1)
        text = text.replace("if (*priv == NULL)\n\t\treturn;", "if (*priv == NULL) { free(priv); return; }", 1)
        text += '''
/* Native port: real per-thread resolver ownership and destructor. */
static pthread_once_t native_asr_once = PTHREAD_ONCE_INIT;
static pthread_key_t native_asr_key;
static int native_asr_key_error;
static void native_asr_key_create(void)
{
    native_asr_key_error = pthread_key_create(&native_asr_key, _asr_resolver_done_tp);
}
static struct asr **native_asr_state(void)
{
    int error = pthread_once(&native_asr_once, native_asr_key_create);
    if (!error) error = native_asr_key_error;
    if (error) { errno = error; return NULL; }
    struct asr **state = pthread_getspecific(native_asr_key);
    if (state) return state;
    state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    error = pthread_setspecific(native_asr_key, state);
    if (error) { free(state); errno = error; return NULL; }
    return state;
}
'''
        path.write_text(text)
        # asr uses only a random 16-bit DNS transaction ID, not legacy BIND state.
        path = generated / "res_send_async.c"
        text = path.read_text().replace("res_randomid()", "arc4random_uniform(65536)")
        path.write_text(text)
        # The DNS transport already distinguishes transient failures, but the
        # address-query parent drops that status. Preserve it in the existing
        # ASYNC_AGAIN flag, which its final no-result state already interprets.
        path = generated / "getaddrinfo_async.c"
        text = path.read_text()
        marker = "\t\tas->as_subq = NULL;\n"
        if text.count(marker) != 1:
            raise ValueError("unexpected address-query subquery completion")
        text = text.replace(marker, marker +
            "\n\t\tif (ar->ar_h_errno == TRY_AGAIN)\n"
            "\t\t\tas->as_flags |= ASYNC_AGAIN;\n")
        path.write_text(text)
        records, objects = [], []
        resource = run(["clang-20", "-print-resource-dir"]).strip()
        flags = ["clang-20", f"--target={args.arch}-apple-macos26.5", "-std=gnu11", "-O2", "-fPIC",
                 "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc", "-D_DARWIN_C_SOURCE",
                 "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
                 "-Wno-deprecated-declarations", "-Wno-availability", "-I", own, "-I", generated, "-I", generated / "include",
                 "-isystem", resource + "/include", "-isystem", sdk / "usr/include",
                 "-isystem", sdk / "usr/local/include", "-include", own / "native_config.h"]
        flags += ["-mcpu=cortex-a72", "-mno-outline-atomics"] if args.arch == "arm64" else ["-march=penryn"]
        def compile_source(original, compiled=None, options=()):
            compiled = compiled or original
            obj = work / (str(len(objects)) + "_" + original.stem + ".o")
            run([*flags, *options, "-c", compiled, "-o", obj])
            magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
            if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if args.arch == "arm64" else 0x1000007, 1):
                raise ValueError("non-native resolver object")
            objects.append(obj)
            records.append({"source": str(original.relative_to(root)),
                            "source_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                            "compiled_sha256": hashlib.sha256(compiled.read_bytes()).hexdigest(),
                            "object": obj.name})
        for name in ("asr.c", "asr_utils.c", "getaddrinfo_async.c", "getnameinfo_async.c", "gethostnamadr_async.c",
                     "res_search_async.c", "res_send_async.c"):
            compile_source(asr / name, generated / name)
        for name, boundary in (("getservent.c", "struct servent_data _servent_data;"),
                               ("getservbyname.c", "struct servent *\ngetservbyname("),
                               ("getservbyport.c", "struct servent *\ngetservbyport(")):
            original = bsd / "lib/libc/net" / name
            text = original.read_text()
            if text.count(boundary) != 1:
                raise ValueError("unexpected OpenBSD services source")
            text = text[:text.index(boundary)].replace("__pledge_open(", "open(")
            compiled = generated / name
            compiled.write_text(text)
            compile_source(original, compiled)
        for name in ("freeaddrinfo.c", "gai_strerror.c"):
            compile_source(bsd / "lib/libc/net" / name)
        original = bsd / "lib/libc/net/res_comp.c"
        text = original.read_text()
        # Retain the exact published hostname validator and its ASCII macros;
        # unrelated legacy packet APIs are outside this provider's public scope.
        text = text[:text.index("static int")] + text[text.index("#define PERIOD"):text.index("DEF_STRONG(__res_hnok);") + len("DEF_STRONG(__res_hnok);")]
        compiled = generated / "res_hnok.c"
        compiled.write_text(text)
        compile_source(original, compiled)
        text = original.read_text()
        host_limit = re.search(r"^#define\s+HOST_NAME_MAX\s+255\b.*$", (bsd / "sys/sys/syslimits.h").read_text(), re.M)
        if not host_limit:
            raise ValueError("upstream compressed-name hostname bound changed")
        text = text[:text.index("static int")] + "\n" + host_limit[0] + "\n#undef dn_expand\n#define dn_expand native_resolver_expand_body\n" + \
            text[text.index("/*\n * Expand compressed domain"):text.index("DEF_WEAK(dn_expand);") + len("DEF_WEAK(dn_expand);")]
        text = text.replace("int\ndn_expand(", "static int\ndn_expand(")
        text += '''
int native_resolver_expand(const unsigned char *begin, const unsigned char *end,
                           const unsigned char *name, char *buffer, int length)
{
    if (!begin || !end || !name || !buffer || length <= 0) return -1;
    return native_resolver_expand_body(begin, end, name, buffer, length);
}
'''
        compiled = generated / "dns_expand.c"
        compiled.write_text(text)
        compile_source(original, compiled)
        for name in ("getifaddrs.c", "if_nametoindex.c", "if_indextoname.c", "vars.c"):
            compile_source(libraries / "Libinfo/gen.subproj" / name,
                           options=["-I", libraries / "Libinfo/Libinfo"])
        compile_source(libraries / "Libinfo/dns.subproj/herror.c",
                       options=["-I", libraries / "Libinfo/Libinfo"])
        for name in ("recallocarray.c", "explicit_bzero.c"):
            original = asr.parent / name
            compiled = generated / name
            shutil.copyfile(original, compiled)
            compile_source(original, compiled)
        compile_source(bsd / "lib/libc/stdlib/reallocarray.c")
        compile_source(libraries / "Libc/stdlib/FreeBSD/strtonum.c")
        compile_source(root / "src/Kernel/xnu/12377.121.6/libsyscall/wrappers/mach_boottime.c",
                       options=["-DPRIVATE=1"])
        compile_source(own / "resolver.c")
        archive = work / "resolver.a"
        run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *objects])
        public, imports, weak = symbols(archive)
        if not {"_getaddrinfo", "_freeaddrinfo", "_gai_strerror"} <= public:
            raise ValueError("resolver API missing")
        args.report.write_text(json.dumps({"architecture": args.arch,
            "backend": "OpenSMTPD 7.8.0p1 OpenBSD asr, OpenBSD services files, Apple interface enumeration",
            "objects": records, "exports": sorted(public), "imports": sorted(imports - public),
            "weak_imports": sorted(weak), "runtime_checked": False,
            "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "port_sources": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in (own / "resolver.c", own / "records.h", own / "native_config.h", own / "build_resolver.py")},
            "upstream_manifests": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in sources.rglob("UPSTREAM.json")},
            "port_adaptations": ["pthread key-owned default contexts and destructor; no shared fallback",
                                 "cancellation disabled for state transitions, enabled around actual poll only",
                                 "OpenBSD-only AI_FQDN disabled to preserve Darwin AI_CANONNAME",
                                 "real native arc4random_uniform supplies 16-bit DNS transaction IDs",
                                 "propagate real DNS TRY_AGAIN into existing address-query ASYNC_AGAIN state",
                                 "real reentrant services parser with native open and private symbol names",
                                 "exact ASCII hostname validator and bounded compressed-name parser extracted",
                                 "query-owned DNS record errors; no process-global BIND state",
                                 "native reverse address queries and per-thread service lookup ownership"],
            "supported_flags": ["AI_PASSIVE", "AI_CANONNAME", "AI_NUMERICHOST", "AI_NUMERICSERV", "AI_ADDRCONFIG"],
            "unsupported_flags": ["AI_ALL", "AI_V4MAPPED", "AI_V4MAPPED_CFG", "AI_UNUSABLE"],
            "configuration": "resolv.conf, hosts, services; no mDNS/DNS-SD or Apple SystemConfiguration",
        }, indent=2) + "\n")
        archive.replace(args.output)
        print(f"Native resolver {args.arch}: {len(objects)} genuine Mach-O objects")


if __name__ == "__main__":
    main()
