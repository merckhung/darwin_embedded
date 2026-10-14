# Copyright (C) 2026 fontconfig Authors
# SPDX-License-Identifier: HPND

from fctest import FcTest, FcTestFont
from pathlib import Path
import pytest


@pytest.fixture
def fctest():
    return FcTest()


@pytest.fixture
def fcfont():
    return FcTestFont()


def test_cache_suffix(fctest, fcfont):
    fctest.setup()
    fctest.install_font(fcfont.fonts, ".")
    cachever = fctest.env.pop("FC_CACHE_VERSION", None)
    cachesnapver = fctest.env.pop("FC_CACHE_SNAP_VERSION", None)
    assert cachever
    assert cachesnapver
    cachever = int(cachever)
    cachesnapver = int(cachesnapver)
    for ret, stdout, stderr in fctest.run_cache([fctest.fontdir.name]):
        assert ret == 0, stderr
    if cachesnapver != 0:
        cachever += 1
    cachesuffix = (
        f".cache-{cachever}{(f'~snap{cachesnapver}') if cachesnapver > 0 else ''}"
    )
    fctest.logger.info(cachesuffix)
    fctest.logger.info([f.name for f in Path(fctest.cachedir.name).glob('*cache*')])
    cache_files = [f.name for f in Path(fctest.cachedir.name).glob(f"*{cachesuffix}")]
    assert len(cache_files) > 0
