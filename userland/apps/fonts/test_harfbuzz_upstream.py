#!/usr/bin/env python3
"""Run the original selected HarfBuzz suites in a Bazel host test.

These host executables are test tools only. Native Mach-O artifacts have
separate architecture, API, provenance and binding audits.
"""
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile

from build_harfbuzz import HEADERS, LIBRARIES, inventory


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, command)) + "\n" + result.stdout + result.stderr)
    return result.stdout


def list_value(recipe, name):
    recipe = re.sub(r"#.*", "", recipe)
    match = re.search(r"^" + name + r"\s*=\s*\[(.*?)^\]", recipe, re.M | re.S)
    if not match:
        raise ValueError("missing original test inventory: " + name)
    return re.findall(r"'([^']+)'", match[1])


def main():
    root = Path.cwd()
    source = root / "src/Applications/fonts/harfbuzz/14.6.0"
    work = Path(os.environ["TEST_TMPDIR"]) / "harfbuzz-upstream"
    work.mkdir()
    library_dir = work / "src"
    library_dir.mkdir()
    os.environ["TMPDIR"] = str(work)
    recipe = (source / "src/meson.build").read_text()
    with tarfile.open(root / "userland/apps/fonts/harfbuzz.arm64.tar") as archive:
        for entry in archive.getmembers():
            if entry.name.startswith(("usr/include/harfbuzz/", "usr/share/harfbuzz/api/")):
                archive.extract(entry, work, filter="data")
    includes = work / "usr/include/harfbuzz"
    configured = json.loads((root / "userland/apps/fonts/harfbuzz.arm64.json").read_text())["configuration"]
    # The host suite probes its own declarations, never the Darwin SDK ABI.
    root_recipe = (source / "meson.build").read_text()
    for header in re.findall(r"\['([^']+)'", re.search(r"^check_headers = \[(.*?)^\]", root_recipe, re.M | re.S)[1]):
        macro = "HAVE_" + re.sub(r"[^A-Z0-9]", "_", header.upper())
        feature = work / "feature.cc"
        feature.write_text("#include <" + header + ">\n")
        result = subprocess.run(["clang++-20", "-std=c++11", "-Werror", "-c", str(feature), "-o", str(work / "feature.o")], capture_output=True)
        if result.returncode: configured.pop(macro, None)
        else: configured[macro] = 1
    prefix = "#include <cstdlib>\n#include <unistd.h>\n#include <sys/mman.h>\n#include <locale.h>\n#include <cmath>\n"
    for function in re.findall(r"\['([^']+)'", re.search(r"^check_funcs = \[(.*?)^\]", root_recipe, re.M | re.S)[1]):
        macro = "HAVE_" + function.upper()
        feature.write_text(prefix + "auto f=&" + function + ";\n")
        result = subprocess.run(["clang++-20", "-std=c++11", "-Werror", "-c", str(feature), "-o", str(work / "feature.o")], capture_output=True)
        if result.returncode: configured.pop(macro, None)
        else: configured[macro] = 1
    (work / "config.h").write_text("".join(f"#define {key} {value}\n" for key, value in sorted(configured.items())))
    packages = ["glib-2.0", "gobject-2.0", "freetype2", "libpng", "zlib"]
    cflags = shlex.split(run(["pkg-config", "--cflags", *packages]))
    ldflags = shlex.split(run(["pkg-config", "--libs", *packages]))
    flags = ["-std=c++11", "-O2", "-fPIC", "-DHAVE_CONFIG_H", "-fno-exceptions", "-fno-rtti",
             "-fno-threadsafe-statics", "-fvisibility-inlines-hidden", "-I", work, "-I", includes,
             "-I", source / "src", *cflags]
    generator = work / "glib-mkenums"
    generator.write_text((root / "src/Applications/glib/glib/2.90.1/gobject/glib-mkenums.in").read_text()
                         .replace("@PYTHON@", "python3").replace("@VERSION@", "2.90.1"))
    headers = [source / "src" / path for variable in HEADERS["libharfbuzz"] for path in inventory(recipe, variable)]
    temporary = work / "hb-gobject-enums.tmp.cc"
    temporary.write_text(run(["python3", generator, "--identifier-prefix", "hb_", "--symbol-prefix", "hb_gobject",
                              "--template", source / "src/hb-gobject-enums.cc.tmpl", *headers, includes / "hb-version.h"]))
    run(["python3", source / "src/fix_get_types.py", temporary, work / "hb-gobject-enums.cc"])
    shaders = re.search(r"^hb_gpu_shader_sources = \[(.*?)^\]", recipe, re.M | re.S)
    for shader, header in re.findall(r"\[\s*'([^']+)',\s*'([^']+)'\s*\]", shaders[1]):
        run(["python3", source / "src/gen-gpu-shader-artifacts.py", work / header, work, source / "src" / shader])
    libraries = {}
    dependencies = {"libharfbuzz": [], "libharfbuzz-gobject": ["libharfbuzz"],
                    "libharfbuzz-subset": ["libharfbuzz"], "libharfbuzz-raster": ["libharfbuzz"],
                    "libharfbuzz-vector": ["libharfbuzz"], "libharfbuzz-gpu": ["libharfbuzz"]}
    for name, variables in LIBRARIES.items():
        files = [source / "src" / path for variable in variables for path in inventory(recipe, variable) if path.endswith(".cc")]
        if name == "libharfbuzz-gobject": files.append(work / "hb-gobject-enums.cc")
        objects = [library_dir / ("hb-" + name + f"_{index:03d}.o") for index in range(len(files))]
        def compile_one(job):
            path, obj = job
            run(["clang++-20", *flags, "-c", path, "-o", obj])
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(compile_one, zip(files, objects)))
        library = library_dir / (name + ".so")
        run(["clang-20", "-shared", "-o", library, *objects,
             *[libraries[key] for key in dependencies[name]], *ldflags, "-pthread", "-lm", "-Wl,-rpath," + str(library_dir)])
        libraries[name] = library
        shutil.copyfile(work / "usr/share/harfbuzz/api" / (name + ".def"), library_dir / (name.removeprefix("lib") + ".def"))
    targets, cases = [], []
    def executable(name, files, options=(), deps=None, suite="src"):
        output = work / name
        targets.append((output, files, options, deps or list(libraries.values())))
        cases.append((suite + "/" + name, [output], source))
        return output
    compiled = re.search(r"compiled_tests = \{(.*?)^  \}", recipe, re.M | re.S)
    for name, entries in re.findall(r"'([^']+)': \[(.*?)\]", compiled[1]):
        files = [source / "src" / path for path in re.findall(r"'([^']+)'", entries)]
        executable("src-" + name, files, ["-DMAIN", "-UNDEBUG"])
    api_recipe = (source / "test/api/meson.build").read_text()
    api_files = list_value(api_recipe, "tests") + ["test-ft.c", "test-multithread.c", "test-raster.cc", "test-gpu.cc"]
    for name in api_files:
        options = ["-DHB_EXPERIMENTAL_API"] if "-instance" in name else []
        executable("api-" + name.rsplit(".", 1)[0], [source / "test/api" / name], options, suite="api")
    for name in ("hb-shape-threads", "hb-subset-threads"):
        executable(name, [source / "test/threads" / (name + ".cc")], suite="threads")
    for name in ("hb-shape", "hb-subset", "hb-vector"):
        output = executable(name, [source / "util" / (name + ".cc")], suite="util")
        cases.pop()  # Utilities are exercised by their complete fixture suites.
    budget = executable("test-vector-budget", [source / "test/vector/test-vector-budget.c"], suite="vector")
    cases[-1] = ("vector/budget", [budget, source / "test/api/fonts/glyphs.ttf"], source)
    for name in ("shape", "subset", "repacker", "raster", "vector", "gpu"):
        executable("hb-" + name + "-fuzzer", [source / "test/fuzzing" / ("hb-" + name + "-fuzzer.cc"),
                   source / "test/fuzzing/main.cc"], suite="fuzzing")
        cases.pop()
    def compile_test(job):
        output, files, options, deps = job
        objects = []
        for index, path in enumerate(files):
            obj = work / (output.name + f"-test-{index}.o")
            language = ["-std=gnu11"] if path.suffix == ".c" else ["-std=c++11"]
            run(["clang-20", *flags, *language, *options, "-c", path, "-o", obj])
            objects.append(obj)
        run(["clang++-20", "-o", output, *objects, *deps, *ldflags, "-pthread", "-lm", "-Wl,-rpath," + str(library_dir)])
    print(f"Compiling {len(targets)} original HarfBuzz test/tool executables", flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(compile_test, targets))
    for name in ("check-c-linkage-decls", "check-externs", "check-header-guards", "check-includes",
                 "check-static-inits", "check-symbols", "check-libstdc++"):
        cases.append(("src/" + name, ["python3", source / "src" / (name + ".py"), source / "src", source, library_dir], source))
    cases.append(("release-notes", ["python3", source / "src/check-release-notes.py", source / "NEWS", "14.6.0"], source))
    for group in ("in-house", "aots", "text-rendering-tests"):
        paths = list_value((source / "test/shape/data" / group / "meson.build").read_text(), group.replace("-", "_") + "_tests")
        cases.append(("shape/" + group, ["python3", source / "test/shape/run-tests.py", work / "hb-shape",
                      *[source / "test/shape/data" / group / "tests" / path for path in paths]], source))
    subset_recipe = (source / "test/subset/meson.build").read_text()
    for path in list_value(subset_recipe, "tests"):
        cases.append(("subset/" + path, ["python3", source / "test/subset/run-tests.py", work / "hb-subset",
                      source / "test/subset/data/tests" / (path + ".tests")], source))
    for path in list_value(subset_recipe, "repack_tests"):
        cases.append(("repack/" + path, ["python3", source / "test/subset/run-repack-tests.py", work / "hb-subset",
                      source / "test/subset/data/repack_tests" / (path + ".tests")], source))
    for path in list_value((source / "test/vector/meson.build").read_text(), "tests"):
        cases.append(("vector/" + path, ["python3", source / "test/vector/run-tests.py", work / "hb-vector",
                      source / "test/vector/data/tests" / (path + ".tests")], source))
    font_paths = sorted(path for path in (source / "test/fuzzing/fonts").rglob("*") if path.is_file())
    subset_paths = sorted(path for path in (source / "test/subset/data/fonts").rglob("*") if path.is_file())
    graph_paths = sorted(path for path in (source / "test/fuzzing/graphs").rglob("*") if path.is_file())
    for name in ("shape", "subset", "repacker", "raster", "vector", "gpu"):
        paths = graph_paths if name == "repacker" else font_paths + (subset_paths if name == "subset" else [])
        for index in range(0, len(paths), 64):
            cases.append((f"fuzzing/{name}-{index // 64}", [work / ("hb-" + name + "-fuzzer"), *paths[index:index + 64]], source))
    environment = dict(os.environ, MALLOC_CHECK_="1", G_DEBUG="gc-friendly",
                       G_TEST_SRCDIR=str(source / "test/api"), G_TEST_BUILDDIR=str(work),
                       NM="llvm-nm-20", OBJDUMP="llvm-objdump-20",
                       HBHEADERS=" ".join(map(str, [*headers, source / "src/hb-version.h"])),
                       HBSOURCES=" ".join(str(source / "src" / path) for variable in (*LIBRARIES["libharfbuzz"], "hb_base_ragel_generated_sources")
                                          for path in inventory(recipe, variable)))
    def check(case):
        name, command, cwd = case
        result = subprocess.run(list(map(str, command)), capture_output=True, text=True, cwd=cwd, env=environment, timeout=600)
        log = work / (name.replace("/", "-") + ".log")
        log.write_text(result.stdout + result.stderr)
        if result.returncode or re.search(r"^not ok\b", result.stdout, re.M):
            return name, result.returncode, log.read_text()
        return name, 0, ""
    print(f"Running {len(cases)} original selected-feature suite cases", flush=True)
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(check, cases))
    failures = [result for result in results if result[1] or result[2]]
    for name, code, output in failures:
        print(f"FAILED {name} ({code})\n{output}", file=sys.stderr)
    logs = [work / (name.replace("/", "-") + ".log") for name, _, _ in cases]
    print(json.dumps({"scope": "host upstream suites, not guest execution", "compiled_executables": len(targets), "cases": len(cases),
        "suite_cases": dict(Counter(name.split("/")[0] for name, _, _ in cases)),
        "tap_assertions": sum(len(re.findall(r"^ok\b", log.read_text(), re.M)) for log in logs),
        "tap_skips": sum(len(re.findall(r"^ok\b.*#\s*SKIP\b", log.read_text(), re.M | re.I)) for log in logs),
        "tap_skip_details": [{"suite": name, "tap": line} for (name, _, _), log in zip(cases, logs)
                             for line in log.read_text().splitlines() if re.search(r"^ok\b.*#\s*SKIP\b", line, re.I)],
        "failures": len(failures), "host_dependency_versions": run(["pkg-config", "--modversion", *packages]).splitlines()}, indent=2))
    if failures: raise SystemExit(1)


if __name__ == "__main__":
    main()
