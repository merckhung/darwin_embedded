#!/bin/sh

set -eux -o pipefail

export CFLAGS="-coverage -ftest-coverage -fprofile-arcs"

export CAIRO_TEST_IGNORE_image_argb32=$(tr '\n' ',' < .gitlab-ci/ignore-image-argb32.txt)
export CAIRO_TEST_IGNORE_image_rgb24=$(tr '\n' ',' < .gitlab-ci/ignore-image-rgb24.txt)
export CAIRO_TEST_IGNORE_image16_rgb24=$(tr '\n' ',' < .gitlab-ci/ignore-image16-rgb24.txt)
export CAIRO_TEST_TARGET=image,image16

meson setup --buildtype=debug _build .
meson compile -C _build

export srcdir=../../test
cd _build/test
xvfb-run ./cairo-test-suite
