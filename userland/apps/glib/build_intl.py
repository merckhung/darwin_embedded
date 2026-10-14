#!/usr/bin/env python3
"""Compile the genuine gettext runtime inside the Bazel dependency action."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import re
import struct
import subprocess

from native_components import run, symbols


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(root, arch, sdk, work, headers, libraries, flags):
    """Install libintl.h/libintl.a and return its source/configuration report."""
    source = root / "src/Applications/glib/gettext/1.0/gettext-runtime/intl"
    gnulib = source / "gnulib-lib"
    generated = work / "intl-config"
    generated.mkdir()
    config = {
        "ENABLE_NLS": 1, "HAVE_CONFIG_H": 1, "HAVE_VISIBILITY": 1,
        "HAVE_POSIX_PRINTF": 1, "HAVE_ICONV": 1, "HAVE_ICONV_H": 1,
        "ICONV_CONST": "", "USE_POSIX_THREADS": 1, "HAVE_PTHREAD_API": 1,
        "HAVE_PTHREAD_RWLOCK": 1, "HAVE_PTHREAD_MUTEX_RECURSIVE": 1,
        "HAVE_GOOD_USELOCALE": 1, "HAVE_WORKING_USELOCALE": 1,
        "HAVE_LC_MESSAGES": 1,
        "HAVE_LANGINFO_CODESET": 1, "HAVE_STDBOOL_H": 1,
        "HAVE_STDINT_H_WITH_UINTMAX": 1, "HAVE_BUILTIN_EXPECT": 1,
        "HAVE_ALLOCA": 1, "HAVE_CFPREFERENCESCOPYAPPVALUE": 0,
        "HAVE_CFLOCALECOPYPREFERREDLANGUAGES": 0,
        "LOCALENAME_ENHANCE_LOCALE_FUNCS": 0, "ENHANCE_LOCALE_FUNCS": 0,
        "SETLOCALE_NULL_ALL_MTSAFE": 0, "SETLOCALE_NULL_ONE_MTSAFE": 0,
        "USE_POSIX_THREADS_WEAK": 0, "USE_ISOC_THREADS": 0,
        "USE_ISOC_AND_POSIX_THREADS": 0, "USE_WINDOWS_THREADS": 0,
        "HAVE_WEAK_SYMBOLS": 0, "WOE32DLL": 0,
        "PACKAGE": '"gettext-runtime"', "PACKAGE_NAME": '"gettext-runtime"',
        "PACKAGE_VERSION": '"1.0"', "VERSION": '"1.0"',
        "INSTALLPREFIX": '"/usr"', "HAVE_SNPRINTF": 1,
        "HAVE_ASPRINTF": 1, "HAVE_WPRINTF": 1, "HAVE_NEWLOCALE": 1,
        "BITSIZEOF_PTRDIFF_T": 64, "BITSIZEOF_SIZE_T": 64,
        "BITSIZEOF_SIG_ATOMIC_T": 32, "BITSIZEOF_WCHAR_T": 32,
        "BITSIZEOF_WINT_T": 32, "HAVE_UNSIGNED_LONG_LONG_INT": 1,
        "HAVE_LONG_LONG_INT": 1, "restrict": "__restrict",
        "FLEXIBLE_ARRAY_MEMBER": "",
    }
    probes = []

    def probe(name, text, required=True):
        path = generated / (name + ".c")
        path.write_text(text)
        obj = path.with_suffix(".o")
        command = [*flags, "-std=gnu11", "-Werror", "-c", path, "-o", obj]
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
        probes.append({"name": name, "source": text, "available": result.returncode == 0})
        if result.returncode and required:
            raise RuntimeError("native libintl feature absent: " + name + "\n" + result.stderr)
        return result.returncode == 0

    required_headers = ("alloca.h", "inttypes.h", "langinfo.h", "locale.h", "search.h",
                        "stdint.h", "stdlib.h", "string.h", "strings.h", "sys/param.h",
                        "sys/stat.h", "sys/mman.h", "unistd.h", "xlocale.h", "wchar.h")
    for header in required_headers:
        macro = "HAVE_" + header.upper().replace("/", "_").replace(".", "_")
        probe(macro, "#include <" + header + ">\n")
        config[macro] = 1
    includes = "".join("#include <" + header + ">\n" for header in
                       ("stdlib.h", "string.h", "strings.h", "unistd.h", "stdio.h",
                        "sys/mman.h", "locale.h", "xlocale.h", "search.h", "wchar.h"))
    for function in ("getcwd", "getegid", "geteuid", "getgid", "getuid", "mmap", "munmap",
                     "stpcpy", "strcasecmp", "uselocale", "newlocale", "querylocale",
                     "asprintf", "snprintf", "wprintf", "tsearch", "tfind", "tdelete"):
        probe(function, includes + "void *native_api=(void*)&" + function + ";\n")
        config["HAVE_" + function.upper()] = 1
    for function in ("mempcpy", "feof_unlocked", "fgets_unlocked"):
        available = probe(function, includes + "void *native_api=(void*)&" + function + ";\n", False)
        if available:
            config["HAVE_" + function.upper()] = 1
        config["HAVE_DECL_" + function.upper()] = int(available)
    probe("pthread_lock_types", "#include <pthread.h>\n"
          "pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;\n"
          "pthread_rwlock_t rwlock=PTHREAD_RWLOCK_INITIALIZER;\n"
          "int recursive=PTHREAD_MUTEX_RECURSIVE;\n")
    probe("iconv", "#include <iconv.h>\nvoid *native_iconv=(void*)&iconv;\n")
    text = (source / "config.h.in").read_text()
    text = re.sub(r"^#undef (\w+)$", lambda match:
                  "#define " + match[1] + " " + str(config[match[1]]) if match[1] in config
                  else "/* #undef " + match[1] + " */", text, flags=re.M)
    (generated / "config.h").write_text(text)
    # Select the exact gnulib stringeq inline from string.in.h while retaining
    # the genuine native string declarations, with no unrelated replacements.
    string_template = (gnulib / "string.in.h").read_text()
    start = string_template.index("_GL_STRING_INLINE bool\nstreq (")
    end = string_template.index("\n}", start) + 2
    stringeq = string_template[start:end]
    (generated / "string.h").write_text(string_template[:string_template.index("*/") + 2] +
        '\n#ifndef NATIVE_GETTEXT_STRING_H\n#define NATIVE_GETTEXT_STRING_H\n'
        '#include_next <string.h>\n#include <stdbool.h>\n' +
        stringeq.replace("_GL_STRING_INLINE", "static inline") + '\n#endif\n')
    # gnulib's locale.in.h includes xlocale.h when HAVE_XLOCALE_H is selected.
    # Both declarations here are the real installed Apple headers.
    (generated / "locale.h").write_text('#ifndef NATIVE_GETTEXT_LOCALE_H\n'
        '#define NATIVE_GETTEXT_LOCALE_H\n#include_next <locale.h>\n#include <xlocale.h>\n#endif\n')
    values = {key: str(config[key]) for key in ("HAVE_POSIX_PRINTF", "HAVE_ASPRINTF", "HAVE_SNPRINTF",
              "HAVE_WPRINTF", "HAVE_NEWLOCALE", "ENHANCE_LOCALE_FUNCS", "HAVE_VISIBILITY", "WOE32DLL")}
    public = (source / "libgnuintl.in.h").read_text()
    for key, value in values.items():
        public = public.replace("@" + key + "@", value)
    if re.search(r"@\w+@", public):
        raise ValueError("unresolved actual libintl header substitution")
    (headers / "libintl.h").write_text(public)
    exported = (source / "export.h").read_text()
    for key, value in values.items():
        exported = exported.replace("@" + key + "@", value)
    private = re.sub(r'extern ([^"])', r'extern LIBINTL_SHLIB_EXPORTED \1', public)
    private = re.sub(r"^(#define _LIBINTL_H[^\n]*)$", lambda match: match[1] + "\n" + exported,
                     private, count=1, flags=re.M)
    (generated / "libgnuintl.h").write_text(private)
    inventory = (source / "Makefile.am").read_text().replace("\\\n", " ")
    match = re.search(r"^LIBINTLSOURCES\s*=\s*(.*)$", inventory, re.M)
    if not match:
        raise ValueError("genuine LIBINTLSOURCES inventory missing")
    names = match[1].split()
    originals = [source / ("plural.c" if name == "plural.y" else name) for name in names]
    helpers = ("localename.c", "localename-environ.c", "localename-unsafe.c",
               "getlocalename_l-unsafe.c", "setlocale_null.c", "setlocale_null-unlocked.c",
               "setlocale-lock.c", "glthread/lock.c", "glthread/once.c")
    originals += [gnulib / name for name in helpers]
    objects = [generated / (f"{index:03}_" + path.stem + ".o") for index, path in enumerate(originals)]
    compile_flags = [*flags, "-std=gnu11", "-DHAVE_CONFIG_H=1", "-DBUILDING_LIBINTL=1",
                     "-DBUILDING_LIBRARY=1", "-fvisibility=hidden",
                     '-DLOCALEDIR="/usr/share/locale"', '-DLIBDIR="/usr/lib"',
                     "-I", generated, "-I", source, "-I", gnulib]

    def compile_one(job):
        path, obj = job
        run([*compile_flags, "-c", path, "-o", obj])
        magic, cpu, _, kind = struct.unpack_from("<4I", obj.read_bytes())
        if (magic, cpu, kind) != (0xfeedfacf, 0x100000c if arch == "arm64" else 0x1000007, 1):
            raise ValueError("non-native libintl object: " + str(path))
        return {"source": str(path.relative_to(root)), "source_sha256": digest(path),
                "compiled_sha256": digest(path), "member": obj.name, "object_sha256": digest(obj)}

    with ThreadPoolExecutor(max_workers=8) as pool:
        records = list(pool.map(compile_one, zip(originals, objects)))
    archive = libraries / "libintl.a"
    run(["llvm-ar-20", "--format=darwin", "rcsD", archive, *objects])
    public, imports, weak = symbols(archive)
    defined = set()
    for line in run(["llvm-nm-20", "-m", "-g", archive]).splitlines():
        if "external " in line and "(undefined)" not in line:
            match = re.search(r"external\s+(?:(?:\([^)]*\)|\[[^]]*\])\s+)*(\S+)", line)
            if not match:
                raise ValueError("unrecognized native intl symbol: " + line)
            defined.add(match[1])
    required = {"_libintl_" + name for name in ("gettext", "dgettext", "dcgettext", "ngettext",
                "dngettext", "dcngettext", "textdomain", "bindtextdomain", "bind_textdomain_codeset")}
    if not required <= public:
        raise ValueError("real gettext public APIs missing: " + str(sorted(required - public)))
    return {"project": "gettext", "version": "1.0", "sources": records,
            "exports": sorted(public), "imports": sorted(imports - defined), "weak_imports": sorted(weak),
            "configuration": config, "feature_checks": probes,
            "archive_sha256": digest(archive), "generated_config_sha256": digest(generated / "config.h"),
            "public_header_sha256": digest(headers / "libintl.h"),
            "symbol_visibility": "upstream CFLAG_VISIBILITY=-fvisibility=hidden; actual export.h exposes public APIs",
            "builder_sha256": digest(root / "userland/apps/glib/build_intl.py"),
            "inventory": "complete LIBINTLSOURCES plus real gnulib locale/pthread support; actual libiconv supplies locale_charset",
            "adaptations": ["distributed plural.c parser used unchanged",
                            "exact gnulib stringeq inline selected around native string declarations",
                            "Darwin locale wrapper includes the actual SDK xlocale.h as gnulib requires"],
            "stringeq_source_sha256": digest(gnulib / "string.in.h"),
            "stringeq_selected_sha256": hashlib.sha256(stringeq.encode()).hexdigest(),
            "optional_services": "CoreFoundation preferences absent; native environment and locale APIs retained",
            "printf": "actual Darwin libc positional printf selected; no replacement printf implementation",
            "runtime_checked": False}
