#! /usr/bin/env python3
# Copyright (C) 2026 fontconfig Authors
# SPDX-License-Identifier: HPND

import pytest
from pathlib import Path
from tempfile import NamedTemporaryFile
from fctest import FcTest, FcTestFont


@pytest.fixture
def fctest():
    return FcTest()


@pytest.fixture
def fcfont():
    return FcTestFont()


def test_issue547(fctest, fcfont):
    extraconffile = NamedTemporaryFile(
        prefix="fontconfig.", suffix=".extra.conf", mode="w", delete_on_close=False
    )
    fctest._extra.append(
        f'<include ignore_missing="yes">{fctest.convert_path(extraconffile.name)}</include>'
    )

    extraconffile.write(
        f"""
<fontconfig>
  <selectfont>
    <rejectfont>
      <glob>{fctest.fontdir.name}</glob>
    </rejectfont>
  </selectfont>
</fontconfig>
"""
    )
    extraconffile.close()

    fctest.setup()
    fctest.install_font(fcfont.fonts, ".")
    for ret, stdout, stderr in fctest.run_cache(
        [fctest.convert_path(fctest.fontdir.name)]
    ):
        assert ret == 0, stderr
        assert "rejected" in stderr
    cache_files = [f.name for f in Path(fctest.cachedir.name).glob("*cache*")]
    assert len(cache_files) == 0
