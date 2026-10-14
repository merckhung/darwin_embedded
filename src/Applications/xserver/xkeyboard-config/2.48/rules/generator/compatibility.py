# SPDX-License-Identifier: MIT

from __future__ import annotations

import functools
import re
import textwrap
from dataclasses import dataclass
from pathlib import Path
from typing import ClassVar, Iterable, Self, Sequence

ROOT = Path(__file__).parent.parent.parent
RULES = ROOT / "rules"
SYMBOLS = ROOT / "symbols"
# Some checks in case we move this script
assert RULES.is_dir(), (
    f"Unexpected directory tree, expected '{RULES}/' - did this script move?"
)
assert SYMBOLS.is_dir(), (
    f"Unexpected directory tree, expected '{SYMBOLS}/' - did this script move?"
)


@functools.total_ordering
@dataclass(frozen=True, order=False)
class Layout:
    PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"(?P<layout>[^(]+)\((?P<variant>[^)]+)\)"
    )
    DEFAULT_MODEL: ClassVar[str] = "*"
    DEFAULT_SYMBOLS_PREFIX: ClassVar[str] = "pc"

    layout: str
    variant: str
    model: str = DEFAULT_MODEL
    symbols_prefix: str = DEFAULT_SYMBOLS_PREFIX

    def __str__(self) -> str:
        if self.variant:
            return f"{self.layout}({self.variant})"
        else:
            return self.layout

    def __lt__(self, other):
        """
        Custom compare function in order to deal with missing variant.
        """
        if not isinstance(other, self.__class__):
            return NotImplemented
        elif self.model != other.model:
            if (self.model == "*") ^ (other.model == "*"):
                return other.model == "*"
            else:
                return self.model < other.model
        elif (self.layout == "*") ^ (other.layout == "*"):
            return other.layout == "*"
        elif self.layout.startswith("$") ^ other.layout.startswith("$"):
            return other.layout.startswith("$")
        elif self.layout == other.layout:
            if self.variant == other.variant:
                return False
            # Handle missing variant
            elif self.variant and (not other.variant or other.variant == "*"):
                return True
            # Handle missing variant
            elif (not self.variant or self.variant == "*") and other.variant:
                return False
            else:
                return self.variant < other.variant
        else:
            return self.layout < other.layout

    @classmethod
    def parse(
        cls,
        raw: str,
        model: str = DEFAULT_MODEL,
        symbols_prefix: str = DEFAULT_SYMBOLS_PREFIX,
    ) -> Self:
        if m := cls.PATTERN.match(raw):
            return cls(
                layout=m.group("layout"),
                variant=m.group("variant"),
                model=model,
                symbols_prefix=symbols_prefix,
            )
        else:
            return cls(
                layout=raw, variant="", model=model, symbols_prefix=symbols_prefix
            )


@dataclass(frozen=True, order=True)
class CompatSymbolsMapping:
    source: Layout
    destination: Layout

    @classmethod
    def parse(cls, raw: str, is_vendor_symbols: bool) -> Self:
        # Drop comment
        raw = raw.split("//")[0]
        parts = raw.split()

        if is_vendor_symbols:
            if len(parts) != 4:
                raise ValueError(raw)
            source = Layout.parse(parts[2], model=parts[0])
            destination = Layout.parse(parts[3], symbols_prefix=parts[1])
        else:
            match len(parts):
                case 2:
                    source = Layout.parse(parts[0])
                    destination = Layout.parse(parts[1])
                case 4:
                    source = Layout(layout=parts[0], variant=parts[1])
                    destination = Layout(layout=parts[2], variant=parts[3])
                case _:
                    raise ValueError(raw)

        return cls(source=source, destination=destination)

    @classmethod
    def parse_iter(
        cls, lines: Iterable[str], is_vendor_symbols: bool
    ) -> Iterable[Self]:
        for line in lines:
            line, *_ = line.split("//")
            if not line:
                continue
            yield cls.parse(line, is_vendor_symbols)

    @classmethod
    def parse_array(cls, raw: str, is_vendor_symbols: bool) -> Iterable[Self]:
        lines = textwrap.dedent(raw).splitlines()
        yield from cls.parse_iter(lines, is_vendor_symbols=is_vendor_symbols)


@dataclass(frozen=True)
class CompatMappings:
    layouts: Sequence[CompatSymbolsMapping]
    variants: Sequence[CompatSymbolsMapping]
    vendors: Sequence[CompatSymbolsMapping]

    @classmethod
    def load(
        cls,
        layouts_path: Path | None = None,
        variants_path: Path | None = None,
        vendors_path: Path | None = None,
        skip_if_source_file_exists: bool = True,
    ) -> Self:
        def check_symbols(layout: Layout, skip_if_source_file_exists: bool) -> bool:
            return skip_if_source_file_exists == (
                not (SYMBOLS / layout.source.layout).is_file()
            )

        if layouts_path:
            with layouts_path.open("rt", encoding="utf-8") as fd:
                layouts = sorted(
                    filter(
                        functools.partial(
                            check_symbols,
                            skip_if_source_file_exists=skip_if_source_file_exists,
                        ),
                        CompatSymbolsMapping.parse_iter(fd, is_vendor_symbols=False),
                    )
                )
        else:
            layouts = []

        if variants_path:
            with variants_path.open("rt", encoding="utf-8") as fd:
                variants = sorted(
                    filter(
                        functools.partial(
                            check_symbols,
                            skip_if_source_file_exists=skip_if_source_file_exists,
                        ),
                        CompatSymbolsMapping.parse_iter(fd, is_vendor_symbols=False),
                    )
                )
        else:
            variants = []

        if vendors_path:
            with vendors_path.open("rt", encoding="utf-8") as fd:
                vendors = sorted(
                    CompatSymbolsMapping.parse_iter(fd, is_vendor_symbols=True)
                )
        else:
            vendors = []

        return cls(layouts=layouts, variants=variants, vendors=vendors)
