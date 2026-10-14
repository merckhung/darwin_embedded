# Copyright (C) 2026 fontconfig Authors
# SPDX-License-Identifier: HPND

"""Cross-version cache format compatibility tests.

Builds a baseline fontconfig from a previous release tag and verifies
that caches written by the current version can be read by the baseline
with identical results.

Old fontconfig accepts cache->version >= FC_CACHE_VERSION_NUMBER, so
the forward direction (new writer → old reader) exercises the real
data path.  The test compares fc-list output between current and
baseline: any difference in font properties indicates the baseline
is misinterpreting the cache data.

Discovery tests (xfail) check whether filename suffix differences
(.cache-9 vs .cache-12) are handled without renaming.

Requires FC_BASELINE_BUILDDIR (pre-built baseline) or FC_BASELINE_TAG
(git tag to build from).  Skips if neither is set.
"""

import os
import re
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

import pytest

from fctest import FcTest, FcTestFont


def _read_cache_version_from_meson_build(path):
    """Extract cacheversion integer from a meson.build file."""
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*cacheversion\s*=\s*['\"]?(\d+)", line)
            if m:
                return int(m.group(1))
    return None


def _make_fctest(builddir):
    """Create an FcTest whose binaries come from *builddir*."""
    saved = os.environ.get("builddir")
    os.environ["builddir"] = builddir
    try:
        return FcTest()
    finally:
        if saved is not None:
            os.environ["builddir"] = saved
        else:
            os.environ.pop("builddir", None)


@pytest.fixture(scope="module")
def baseline_builddir(request):
    """Locate or build the baseline fontconfig; return its builddir path."""
    bdir = os.environ.get("FC_BASELINE_BUILDDIR")
    if bdir:
        return bdir

    tag = os.environ.get("FC_BASELINE_TAG")
    if not tag:
        pytest.skip("FC_BASELINE_BUILDDIR or FC_BASELINE_TAG not set")

    srcdir = os.environ.get("srcdir", str(Path(__file__).parents[1]))
    build_py = str(Path(srcdir) / ".gitlab-ci" / "build.py")
    upstream = (
        "https://gitlab.freedesktop.org/"
        f"{os.environ.get('FDO_UPSTREAM_REPO', 'fontconfig/fontconfig')}.git"
    )

    tmpgit_td = TemporaryDirectory(prefix="fc-baseline-git.")
    request.addfinalizer(tmpgit_td.cleanup)
    subprocess.run(
        ["git", "init", "--bare", tmpgit_td.name],
        capture_output=True, check=True,
    )
    subprocess.run(
        ["git", "-C", tmpgit_td.name, "fetch", "--depth=1",
         upstream, "tag", tag],
        check=True,
    )

    src_td = TemporaryDirectory(prefix="fc-baseline-src.")
    request.addfinalizer(src_td.cleanup)
    archive = subprocess.Popen(
        ["git", "-C", tmpgit_td.name, "archive", tag],
        stdout=subprocess.PIPE,
    )
    subprocess.run(
        ["tar", "-C", src_td.name, "-x"],
        stdin=archive.stdout, check=True,
    )
    archive.wait()
    assert archive.returncode == 0, f"git archive {tag} failed"

    bld = str(Path(src_td.name) / "build")
    pfx = str(Path(src_td.name) / "prefix")
    env = os.environ.copy()
    env["BUILDDIR"] = bld
    env["PREFIX"] = pfx
    subprocess.run(
        [sys.executable, build_py, "--source-dir", src_td.name,
         "-C", "-I", "-V", "-d", "nls", "-d", "doc"],
        env=env, check=True,
    )
    return bld


@pytest.fixture(scope="module")
def baseline_cache_version(baseline_builddir):
    """Cache version of the baseline build."""
    cv_str = os.environ.get("FC_BASELINE_CACHE_VERSION")
    if cv_str:
        return int(cv_str)
    src = Path(baseline_builddir).parent
    cv = _read_cache_version_from_meson_build(src / "meson.build")
    if cv is not None:
        return cv
    pytest.fail("Cannot determine baseline cache version")


@pytest.fixture
def current_cache_version():
    """Cache version of the current build."""
    cv = os.environ.get("FC_CACHE_VERSION")
    if cv:
        return int(cv)
    srcdir = os.environ.get("srcdir", str(Path(__file__).parents[1]))
    return _read_cache_version_from_meson_build(Path(srcdir) / "meson.build")


@pytest.fixture
def fctest():
    return FcTest()


@pytest.fixture
def fcfont():
    return FcTestFont()


def _cache_suffix(version):
    return f".cache-{version}"


def _cache_files_with_suffix(cachedir, suffix):
    return list(Path(cachedir).glob(f"*{suffix}"))


def _symlink_caches(cachedir, from_suffix, to_suffix):
    """Create symlinks: for each *from_suffix* file, link a *to_suffix* name."""
    for f in _cache_files_with_suffix(cachedir, from_suffix):
        target = f.parent / f.name.replace(from_suffix, to_suffix)
        if not target.exists():
            target.symlink_to(f.name)


def _snapshot_cache_inodes(cachedir):
    """Return a dict mapping cache filename → (inode, mtime_ns)."""
    return {
        f.name: (f.stat().st_ino, f.stat().st_mtime_ns)
        for f in Path(cachedir).glob("*cache*")
    }


def _parse_verbose_output(text):
    """Parse fc-list -v output into a dict keyed by file path.

    Each pattern becomes a dict mapping property names to their
    raw value strings, keyed by the ``file`` property so patterns
    from different versions can be matched regardless of order.
    """
    patterns = {}
    current = {}
    for line in text.splitlines():
        if line.startswith("Pattern has "):
            if current and "file" in current:
                patterns[current["file"]] = current
            current = {}
        elif line.startswith("\t") and ":" in line:
            key, _, val = line.strip().partition(":")
            current[key.strip()] = val.strip()
    if current and "file" in current:
        patterns[current["file"]] = current
    return patterns


# Properties whose values legitimately differ across fontconfig
# versions even when the cache format is compatible.
_SKIP_PROPERTIES = {
    "lang",
}


def _setup_and_share(fctest, fcfont, baseline_builddir):
    """Set up fonts/config via fctest, return a baseline FcTest sharing it."""
    fctest.setup()
    fctest.install_font(fcfont.fonts, ".")
    baseline = _make_fctest(baseline_builddir)
    baseline._env["FONTCONFIG_FILE"] = fctest._env["FONTCONFIG_FILE"]
    return baseline


# ---- CI-gating test: forward binary format compatibility ----


@pytest.mark.skipif(os.getenv("EXEEXT", "") != "", reason="not working on Win32")
def test_binary_format_forward_compat(
    baseline_builddir, baseline_cache_version, current_cache_version,
    fctest, fcfont,
):
    """Cache written by current fc-cache must be readable by baseline fc-list.

    Compares verbose fc-list output between current and baseline.
    Properties that both versions share must produce identical values;
    any divergence means the baseline is misinterpreting the cache.
    """
    baseline = _setup_and_share(fctest, fcfont, baseline_builddir)
    cachedir = fctest.cachedir.name
    old_cv = baseline_cache_version
    new_cv = current_cache_version

    for ret, _, stderr in fctest.run_cache([fctest.fontdir.name]):
        assert ret == 0, f"Current fc-cache failed: {stderr}"

    new_suffix = _cache_suffix(new_cv)
    assert _cache_files_with_suffix(cachedir, new_suffix), (
        f"No {new_suffix} cache files generated"
    )

    for ret, current_out, stderr in fctest.run_list(["-v"]):
        assert ret == 0, f"Current fc-list failed: {stderr}"

    if old_cv != new_cv:
        _symlink_caches(cachedir, new_suffix, _cache_suffix(old_cv))

    snap = _snapshot_cache_inodes(cachedir)

    for ret, baseline_out, stderr in baseline.run_list(["-v"]):
        assert ret == 0, f"Baseline fc-list failed: {stderr}"
        assert baseline_out.strip(), "Baseline fc-list returned no output"

    snap_after = _snapshot_cache_inodes(cachedir)
    assert snap == snap_after, (
        "Baseline fc-list regenerated cache instead of using current's"
    )

    current_patterns = _parse_verbose_output(current_out)
    baseline_patterns = _parse_verbose_output(baseline_out)

    assert len(baseline_patterns) == len(current_patterns), (
        f"Font count mismatch: baseline saw {len(baseline_patterns)}, "
        f"current saw {len(current_patterns)}"
    )

    for file_key, cur in current_patterns.items():
        assert file_key in baseline_patterns, (
            f"Baseline did not find font {file_key}"
        )
        base = baseline_patterns[file_key]
        shared_keys = (
            set(cur.keys()) & set(base.keys()) - _SKIP_PROPERTIES
        )
        for key in sorted(shared_keys):
            assert base[key] == cur[key], (
                f"Font {cur.get('family', '?')}, property '{key}': "
                f"baseline={base[key]!r}, current={cur[key]!r}"
            )


# ---- Discovery tests: xfail until symlinks/probing implemented ----


@pytest.mark.skipif(os.getenv("EXEEXT", "") != "", reason="not working on Win32")
def test_discovery_backward_compat(
    baseline_builddir, baseline_cache_version, current_cache_version,
    fctest, fcfont,
):
    """Old cache must be discoverable by new fc-list without renaming."""
    old_cv = baseline_cache_version
    new_cv = current_cache_version
    if old_cv != new_cv:
        pytest.xfail(
            f"Discovery mismatch: baseline .cache-{old_cv} vs "
            f"current .cache-{new_cv}"
        )

    baseline = _setup_and_share(fctest, fcfont, baseline_builddir)
    cachedir = fctest.cachedir.name

    for ret, _, stderr in baseline.run_cache([fctest.fontdir.name]):
        assert ret == 0, f"Baseline fc-cache failed: {stderr}"

    snap = _snapshot_cache_inodes(cachedir)

    for ret, stdout, stderr in fctest.run_list(["-", "family"]):
        assert ret == 0, f"Current fc-list failed: {stderr}"
        assert stdout.strip(), "fc-list returned no output"

    snap_after = _snapshot_cache_inodes(cachedir)
    assert snap == snap_after, (
        "Current fc-list regenerated cache -- discovery failed"
    )


@pytest.mark.skipif(os.getenv("EXEEXT", "") != "", reason="not working on Win32")
def test_discovery_forward_compat(
    baseline_builddir, baseline_cache_version, current_cache_version,
    fctest, fcfont,
):
    """New cache must be discoverable by old fc-list without renaming.

    Current fc-cache creates compat symlinks (.cache-9, .cache-10, ...)
    pointing to the real cache file, so old fontconfig can find them.
    """
    baseline = _setup_and_share(fctest, fcfont, baseline_builddir)
    cachedir = fctest.cachedir.name

    for ret, _, stderr in fctest.run_cache([fctest.fontdir.name]):
        assert ret == 0, f"Current fc-cache failed: {stderr}"

    old_cv = baseline_cache_version
    new_cv = current_cache_version
    if old_cv != new_cv:
        old_suffix = _cache_suffix(old_cv)
        symlinks = _cache_files_with_suffix(cachedir, old_suffix)
        assert symlinks, (
            f"fc-cache did not create compat symlinks for .cache-{old_cv}"
        )
        for s in symlinks:
            assert s.is_symlink(), (
                f"{s.name} should be a symlink, not a regular file"
            )

    snap = _snapshot_cache_inodes(cachedir)

    for ret, stdout, stderr in baseline.run_list(["-", "family"]):
        assert ret == 0, f"Baseline fc-list failed: {stderr}"
        assert stdout.strip(), "Baseline fc-list returned no output"

    snap_after = _snapshot_cache_inodes(cachedir)
    assert snap == snap_after, (
        "Baseline fc-list regenerated cache -- discovery failed"
    )
