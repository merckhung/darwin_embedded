# SPDX-License-Identifier: MIT

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from dataclasses import dataclass
from enum import StrEnum, unique
from pathlib import Path

from .parser import MLVO, RulesFile, RulesSet
from .registry import Registry


def error(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    print("Aborting now")
    sys.exit(1)


@unique
class Section(StrEnum):
    """
    XKB sections.
    Name correspond to the header (`xkb_XXX`), value to the subdir/rules header.
    """

    keycodes = "keycodes"
    compatibility = "compat"
    geometry = "geometry"
    symbols = "symbols"
    types = "types"

    @classmethod
    def parse(cls, raw: str) -> Section:
        # Note: in order to display a nice message, argparse requires the error
        # to be one of: ArgumentTypeError, TypeError, or ValueError
        # See: https://docs.python.org/3/library/argparse.html#type
        try:
            return cls[raw]
        except KeyError:
            raise ValueError(raw)

    def get_options(
        self, xkb_root: Path, registries: Iterable[Registry], rules: str
    ) -> Iterable[tuple[str, str]]:
        all_options = (Option(opt) for r in registries for opt in r.options)

        skip = frozenset(self.find_options_to_skip(rules))

        directives = (
            o.resolve(xkb_root)
            for o in sorted(all_options)
            if o.name not in skip and not o.name.startswith("custom:")
        )

        def check_and_map(directive: DirectiveSet) -> Directive:
            assert not directive.is_empty, (
                f"Option {directive.option} does not resolve to any section"
            )

            return getattr(directive, self.name)

        filtered = filter(
            lambda y: y is not None,
            map(check_and_map, directives),
        )

        for d in filtered:
            assert d is not None
            yield (d.name, d)

        if self is Section.types:
            yield ("custom:types", "custom")

    @classmethod
    def find_options_to_skip(cls, rules: str) -> Iterable[str]:
        """
        Find options to skip

        Theses are the “option” rules defined explicitly in partial rules files *.part
        """
        for rs in RulesFile.parse(rules.splitlines()):
            if not isinstance(rs, RulesSet) or MLVO.Option not in rs.mlvo:
                # Skip comments/groups/includes and rulesets without options
                continue
            idx = rs.mlvo.index(MLVO.Option)
            for r in rs.rules:
                yield r.mlvo[idx]


@dataclass
class Directive:
    option: Option
    filename: str
    section: str

    @property
    def name(self) -> str:
        return self.option.name

    def __str__(self) -> str:
        if self.section:
            return f"{self.filename}({self.section})"
        else:
            return self.filename


@dataclass
class DirectiveSet:
    option: Option
    keycodes: Directive | None
    compatibility: Directive | None
    geometry: Directive | None
    symbols: Directive | None
    types: Directive | None

    @property
    def is_empty(self) -> bool:
        return all(
            x is None
            for x in (
                self.keycodes,
                self.compatibility,
                self.geometry,
                self.symbols,
                self.types,
            )
        )


@dataclass
class Option:
    """
    Wrapper around a single option -> KcCGST rules file entry. Has the properties
    name and directive where the directive consists of the XKB symbols file name
    and corresponding section, usually composed in the rules file as:
        name = +directive
    """

    name: str

    def __lt__(self, other) -> bool:
        return self.name < other.name

    @property
    def directive(self) -> Directive:
        f, s = self.name.split(":")
        return Directive(self, f, s)

    def resolve(self, xkb_root: Path) -> DirectiveSet:
        directives: dict[Section, Directive | None] = {s: None for s in Section}
        directive = self.directive
        filename, section_name = directive.filename, directive.section
        for section in Section:
            subdir = xkb_root / section
            if not (subdir / filename).exists():
                # Some of our foo:bar entries map to a baz_vndr/foo file
                for vndr in subdir.glob("*_vndr"):
                    vndr_path = vndr / filename
                    if vndr_path.exists():
                        filename = vndr_path.relative_to(subdir).as_posix()
                        break
                else:
                    continue

            if (subdir / filename).is_symlink():
                resolved_filename = (subdir / filename).resolve().name
                assert (subdir / filename).exists()
            else:
                resolved_filename = filename

            # Now check if the target file actually has that section
            f = subdir / resolved_filename
            with f.open("rt", encoding="utf-8") as fd:
                section_header = f'xkb_{section.name} "{section_name}"'
                if any(section_header in line for line in fd):
                    directives[section] = Directive(
                        self, resolved_filename, section_name
                    )

        return DirectiveSet(
            option=self,
            keycodes=directives[Section.keycodes],
            compatibility=directives[Section.compatibility],
            geometry=directives[Section.geometry],
            symbols=directives[Section.symbols],
            types=directives[Section.types],
        )
