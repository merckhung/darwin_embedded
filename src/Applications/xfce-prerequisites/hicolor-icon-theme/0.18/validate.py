#!/usr/bin/env python3
#
# Copyright (C) 2022-2024 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import sys
import shutil
import subprocess
from contextlib import contextmanager


SPEC_FILES = ['icon-naming-spec.xml', 'icon-theme-spec.xml']
VALIDATE_TABLES = False


@contextmanager
def temp_dir(basename=None):
    import tempfile

    current_dir = os.getcwd()
    temp_dir = tempfile.mkdtemp(prefix='tmp_', dir=current_dir)
    try:
        yield temp_dir
    finally:
        shutil.rmtree(temp_dir)


def main():
    daps_exe = shutil.which('daps')
    if not daps_exe:
        print("daps is not installed - please install it to continue!", file=sys.stderr)
        return 4

    with temp_dir() as tdir:
        xml_target = os.path.join(tdir, 'xml')
        os.makedirs(xml_target, exist_ok=True)

        # copy files into a structure that DAPS likes
        for fname in SPEC_FILES:
            shutil.copy(os.path.join('spec', fname), xml_target)

        # validate
        for fname in SPEC_FILES:
            print('➤', 'Validating:', fname)

            cmd = [daps_exe,
                   '-m', os.path.join(xml_target, fname),
                   'validate']
            if not VALIDATE_TABLES:
                cmd.append('--not-validate-tables')

            res = subprocess.run(cmd, check=False)
            if res.returncode != 0:
                return res.returncode

    return 0


if __name__ == '__main__':
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    sys.exit(main())
