#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
from pathlib import Path

from . import generate_rules, generate_symbols
from .parser import Version


def run_generate_rules(args: argparse.Namespace):
    print(
        *generate_rules(
            args.template,
            version=args.version,
            ruleset=args.ruleset,
            compat=args.compat,
            debug=args.debug,
        ),
        sep="",
        end="",
        file=args.output,
    )


def run_generate_symbols(args: argparse.Namespace):
    for symbol_file in generate_symbols(destination=args.dest):
        # Print path to stdout, for path collection in meson
        print(symbol_file.path.name)
        if args.dry_run:
            continue
        with symbol_file.path.open("wt", encoding="utf-8") as fd:
            fd.write(symbol_file.content)


parser = argparse.ArgumentParser("XKB rules generator")

subparsers = parser.add_subparsers()

rules_parser = subparsers.add_parser("rules", help="Generates rules")
rules_parser.add_argument(
    "template",
    type=argparse.FileType("rt", encoding="utf-8"),
    help="Rules template file",
)
rules_parser.add_argument(
    "--ruleset",
    type=str,
    choices=("base", "evdev"),
    default="evdev",
    help="Base rules (default: %(default)s)",
)
rules_parser.add_argument("--compat", action="store_true", help="Compatibility rules")
rules_parser.add_argument(
    "--version", type=Version.parse, default=Version.V2, help="Format version"
)
rules_parser.add_argument("--debug", action="store_true", help="Debug mode")
rules_parser.add_argument(
    "--output", type=argparse.FileType("w", encoding="UTF-8"), help="Output"
)
rules_parser.set_defaults(func=run_generate_rules)

symbols_parser = subparsers.add_parser(
    "symbols", help="Generates compatibility symbols"
)
symbols_parser.add_argument("dest", type=Path, help="Destination directory")
symbols_parser.add_argument("--dry-run", action="store_true", help="Dry run")
symbols_parser.set_defaults(func=run_generate_symbols)

args = parser.parse_args()

args.func(args)
