# Copyright (C) 2025 fontconfig Authors
# SPDX-License-Identifier: HPND

from fctest import FcTest, FcExternalTestFont, FcTestFont, pytest_generate_tests
from pathlib import Path
from tempfile import NamedTemporaryFile
import pytest


@pytest.fixture
def fctest():
    return FcTest()


@pytest.fixture
def fcfont():
    return FcTestFont()


def test_genconf(fctest, fcfont, parametrized_external_font):
    font_path = Path(parametrized_external_font)

    extraconffile = NamedTemporaryFile(
        prefix="fontconfig.", suffix=".extra.conf", mode="w", delete_on_close=False
    )
    fctest._extra.append(
        f'<include ignore_missing="yes">{fctest.convert_path(extraconffile.name)}</include>'
    )
    fctest.setup()
    fctest.install_font(fcfont.fonts, ".")
    fctest.install_font(parametrized_external_font, ".")

    gl = []
    f = fmt = None
    for ret, stdout, stderr in fctest.run_query(
        ["-f", "%{family[0]}:%{genericfamily}\n", parametrized_external_font]
    ):
        assert ret == 0, stderr
        for l in stdout.strip().splitlines():
            f, g = l.split(":")
            assert f, stdout
            if not g or g == "0":
                # fallback if missing
                gl = ["2"]
                fmt = ":family=sans-serif"
            else:
                for gg in g.split(","):
                    gl += [gg]
                fmt = f":genericfamily={','.join(g)}"

    for ret, stdout, stderr in fctest.run_genconf(
        [item for s in gl for item in ("-g", s)]
        + ["-f", f, "-o", extraconffile.name, parametrized_external_font]
    ):
        assert ret == 0, stderr

    for ret, stdout, stderr in fctest.run_match(["-f", "%{file}\n", fmt]):
        assert ret == 0, stderr
        if Path(stdout.strip().splitlines()[0]).name != font_path.name:
            assert ret == 0, stderr

    for ret, stdout, stderr in fctest.run_scan(
        ["-f", "%{genericfamily}\n", parametrized_external_font]
    ):
        assert ret == 0, stderr
        for l in stdout.strip().splitlines():
            l = l.split(",")
            assert gl == l, stdout
