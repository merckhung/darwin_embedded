# SPDX-License-Identifier: MIT

"""
Rules parser
"""

from __future__ import annotations

import dataclasses
import math
import re
import textwrap
from dataclasses import dataclass
from enum import IntEnum, StrEnum, auto, unique
from functools import partial
from typing import ClassVar, Iterable, Self


def natural_sort_key(s, _nsre=re.compile(r"([0-9]+)")):
    return tuple(
        int(chunk) if chunk.isdigit() else (chunk.lower(), chunk)
        for chunk in _nsre.split(s)
    )


@unique
class Version(IntEnum):
    V2 = 2
    "Compatible with X11"
    V3 = 3

    @classmethod
    def parse(cls, raw: str) -> Self:
        for v in cls:
            if str(v) == raw or f"v{v}" == raw:
                return v
        raise ValueError(raw)


@unique
class MLVO(StrEnum):
    Model = auto()
    Layout = auto()
    Variant = auto()
    Option = auto()

    @classmethod
    def parse(cls, raw: str) -> Self:
        for c in cls:
            if c == raw:
                return c
        raise ValueError(raw)


class LayoutIndex(int):
    def numeric_indices(self, max: int) -> Iterable[int]:
        yield self


@unique
class LayoutRange(StrEnum):
    Single = auto()
    First = auto()
    Later = auto()
    Any = auto()

    @classmethod
    def parse(cls, raw: str) -> Self | LayoutIndex:
        try:
            return LayoutIndex(int(raw, base=10))
        except ValueError:
            pass
        for i in cls:
            if i == raw:
                return i
        raise ValueError()

    def numeric_indices(self, max: int) -> Iterable[int]:
        match self:
            case self.Single:
                yield 0
            case self.First:
                yield 0
                yield 1
            case self.Later:
                yield from range(2, max + 1)
            case self.Any:
                yield from range(0, max + 1)
            case _:
                raise ValueError(self)


@unique
class KcCGST(StrEnum):
    Keycodes = auto()
    Compat = auto()
    Geometry = auto()
    Symbols = auto()
    Types = auto()

    @classmethod
    def parse(cls, raw: str) -> Self:
        for c in cls:
            if c == raw:
                return c
        raise ValueError(raw)


@dataclass(frozen=True)
class Rule:
    """
    A rule associates a MLVO matcher to a section and its value.
    """

    RULE_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"""
        ^
        (?P<mlvo>(?:\s+[^!\s=]+)+)
        \s+=\s+
        (?P<section>\S+)
        \s*$
        """,
        re.VERBOSE,
    )

    SECTION_INDICES: ClassVar[re.Pattern[str]] = re.compile(
        r"(?P<index>\[%i\])|(?P<modifier>:%i)"
    )

    mlvo: tuple[str]
    section: str

    @classmethod
    def parse(cls, raw: str) -> Self:
        if (m := cls.RULE_PATTERN.match(raw)) is None:
            raise ValueError(raw)
        mlvo = tuple(m.group("mlvo").split())
        return cls(mlvo=mlvo, section=m.group("section"))

    @staticmethod
    def _to_numeric_index(k: int, m: re.Match[str]) -> str:
        if k <= 0:
            return ""
        elif m.group("index"):
            return f"[{k}]"
        else:
            return f":{k}"

    def to_numeric_index(self, k: int) -> Self:
        section = self.SECTION_INDICES.sub(
            partial(self._to_numeric_index, k), self.section
        )
        return dataclasses.replace(self, section=section)


@dataclass
class RulesSet:
    """
    A rules set: group of rules with same MLVO and section type.
    """

    RULES_HEADER_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"""
        ^!
        (?P<mlvo>(?:\s+(?:model|layout|variant|option)(?:\[(?:\d+|single|first|later|any)\])?)+)
        \s+=\s+
        (?P<kccgst>\w+)
        \s*$
        """,
        re.VERBOSE,
    )

    MLVO_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"(?P<component>\w+)\[(?P<range>\w+)\]"
    )

    TAB_SIZE: ClassVar[int] = 8
    HEADER_START: ClassVar[str] = "! "
    LEADING_SPACES_COUNT: ClassVar[int] = len(HEADER_START)
    LEADING_SPACES: ClassVar[str] = LEADING_SPACES_COUNT * " "

    mlvo: tuple[MLVO, ...]
    layout_range: LayoutRange | LayoutIndex
    kccgst: KcCGST
    rules: tuple[Rule, ...]

    @classmethod
    def parse_header(
        cls, raw: str
    ) -> tuple[tuple[MLVO, ...], LayoutRange | int, KcCGST] | None:
        if (m := cls.RULES_HEADER_PATTERN.match(raw)) is None:
            return None

        mlvo, layout_index = cls.parse_mvlo(m.group("mlvo"))
        kccgst = KcCGST.parse(m.group("kccgst"))
        return mlvo, layout_index, kccgst

    @classmethod
    def parse_mvlo(cls, raw: str) -> tuple[tuple[MLVO], LayoutRange | LayoutIndex]:
        mlvo: list[MLVO] = []
        layout_range: LayoutRange | LayoutIndex | None = None
        for c in raw.split():
            # TODO: check layout range consistency
            if m := cls.MLVO_PATTERN.match(c):
                component = MLVO.parse(m.group("component"))
                layout_range = LayoutRange.parse(m.group("range"))
            else:
                component = MLVO.parse(c)
            mlvo.append(component)
        if layout_range is None:
            layout_range = LayoutRange.Single
        return tuple(mlvo), layout_range

    def to_numeric_index(self, k: int) -> Self:
        rules = list(r.to_numeric_index(k) for r in self.rules)
        return dataclasses.replace(self, layout_range=k, rules=rules)

    def to_numeric_indices(self, max: int) -> Iterable[Self]:
        yield from map(self.to_numeric_index, self.layout_range.numeric_indices(max))

    def serialize_mlvo(self) -> Iterable[str]:
        for c in self.mlvo:
            if (c is MLVO.Layout or c is MLVO.Variant) and (self.layout_range != 0):
                yield f"{c}[{self.layout_range}]"
            else:
                yield c

    def serialize(self, version: Version, debug: bool = False) -> Iterable[str]:
        if version is Version.V2:
            for k, rs in enumerate(self.to_numeric_indices(4)):
                if k > 0:
                    yield "\n"
                yield from rs._serialize(debug=debug)
        else:
            yield from self._serialize(debug=debug)

    def _serialize(self, debug: bool) -> Iterable[str]:
        # Pretty-printing: Find the maximal length of each component.
        mlvo = tuple(self.serialize_mlvo())
        mlvo = (self.HEADER_START + mlvo[0],) + mlvo[1:]
        sizes = [len(c) + 1 for c in mlvo]
        groups = [False for _ in mlvo]
        for rule in self.rules:
            for k, c in enumerate(rule.mlvo):
                if has_group := Group.is_group_name(c):
                    groups[k] = True
                if k == 0:
                    xtra_space = self.LEADING_SPACES_COUNT - (1 if has_group else 0)
                else:
                    xtra_space = 1 if has_group else 0
                sizes[k] = max(sizes[k], len(c) + 1 + xtra_space)

        # Update sizes and print header
        for k, (c, s, g) in enumerate(zip(mlvo, sizes, groups)):
            s = sizes[k] = math.ceil(s / self.TAB_SIZE) * self.TAB_SIZE
            yield self.append_tabs(" " + c if g and k > 0 else c, s)
        yield "=\t"
        yield self.kccgst
        yield "\n"

        # Print rules
        def prepend_space(c: str, k: int):
            if Group.is_group_name(c):
                if k == 0:
                    return self.LEADING_SPACES[:-1] + c
                else:
                    return c
            elif k == 0:
                return self.LEADING_SPACES + c
            elif groups[k]:
                return " " + c
            else:
                return c

        for rule in self.rules:
            if debug:
                yield f"  // "
                yield rule
                yield "\n"
            yield from (
                self.append_tabs(prepend_space(c, k), s)
                for k, (c, s) in enumerate(zip(rule.mlvo, sizes))
            )
            yield "=\t"
            yield rule.section
            yield "\n"

    @classmethod
    def append_tabs(cls, text: str, target_length: int) -> str:
        """
        Appends tabs to reach the target string length.
        """
        length = len(text)
        if length > target_length:
            return text
        else:
            count = math.ceil((target_length - length) / cls.TAB_SIZE)
            return text + count * "\t"


@dataclass
class Include:
    """
    Include statement in rules files
    """

    INCLUDE_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"""^!\s+include\s+(?P<file>[^\s"]+)\s*$"""
    )

    file: str

    @classmethod
    def parse(cls, line: str) -> Self | None:
        if m := cls.INCLUDE_PATTERN.match(line):
            file = m.group("file")
            return cls(file)
        else:
            return None

    def serialize(self, *args, **kwargs) -> Iterable[str]:
        yield f'! include "{self.file}\n"'


@dataclass
class Group:
    """
    Group of items used in rules
    """

    GROUP_HEADER_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"""
        ^!\s+
        (?P<name>\$(?:\w|-)+)
        \s+=
        (?P<members>(?:\s+(?:\w|-)+)+)
        \s*$
        """,
        re.VERBOSE,
    )

    name: str
    "Groups name starts with $"
    members: set[str]
    component: MLVO | None = None
    description: str = ""

    @classmethod
    def parse(cls, line: str) -> Self | None:
        if m := cls.GROUP_HEADER_PATTERN.match(line):
            name = m.group("name")
            members = m.group("members").split()
            return cls(name=name, members=set(members))
        else:
            return None

    def _serialize(self) -> str:
        return f"! {self.name} = {' '.join(sorted(self.members, key=natural_sort_key))}"

    def serialize(self, *args, **kwargs) -> Iterable[str]:
        disabled = not self.members
        group = self._serialize()
        eq_index = group.index("=") + 2
        group = " \\\n".join(
            textwrap.wrap(group, width=78, subsequent_indent=eq_index * " ")
        )
        if disabled:
            group = textwrap.indent(group, "//")
        if self.description:
            description = textwrap.indent(self.description, "// ")
            group = f"{description}\n{group}"
        yield group
        yield "\n"

    @staticmethod
    def is_group_name(s: str) -> bool:
        return s.startswith("$")


@dataclass
class Comment:
    """
    A generic comment
    """

    text: str = ""

    def __bool__(self) -> bool:
        return bool(self.text)

    def __add__(self, other):
        if not isinstance(other, self.__class__):
            return NotImplemented
        return dataclasses.replace(self, text=self._add(other.text))

    def __iadd__(self, other):
        if isinstance(other, str):
            self.text = self._add(other)
        elif isinstance(other, self.__class__):
            self.text = self._add(other.text)
        else:
            return NotImplemented
        return self

    def _add(self, text: str) -> str:
        return f"{self.text}\n{text}"

    @classmethod
    def parse(cls, line: str) -> tuple[str, Self] | None:
        parts = line.split("//", maxsplit=1)
        if len(parts) > 1:
            return (parts[0], cls(parts[1]))
        else:
            return None

    def serialize(self, *args, **kwargs) -> Iterable[str]:
        yield textwrap.indent(self.text + "\n", "//", lambda _: True)


@dataclass
class BlankLine:
    def serialize(self, *args, **kwargs) -> Iterable[str]:
        yield "\n"


@dataclass
class RulesFile:
    """
    A rules file
    """

    @classmethod
    def parse(
        cls, raw: Iterable[str]
    ) -> Iterable[Include | Group | RulesSet | Comment | BlankLine]:
        rules_set: RulesSet | None = None
        pending_comment: Comment | None = None
        pending_line: str = ""
        pending_line_number: int | None = None
        for line_number, raw_line in enumerate(raw, start=1):
            # Handle comment
            parts = raw_line.split("//", maxsplit=1)
            line, *rest = parts
            if rest:
                comment: Comment | None = Comment(rest[0].rstrip())
            else:
                comment = None
            # Handle EOL escape
            parts = line.split("\\")
            if (length := len(parts)) > 1:
                # Only valid if actually at the end of the line
                if length > 2 or (parts[-1] and parts[-1] != "\n"):
                    raise ValueError(line_number, parts, raw_line)
                else:
                    if pending_line_number is None:
                        pending_line_number = line_number
                    pending_line += raw_line[:-2]
                    continue

            # Complete pending line
            line = pending_line + line
            pending_line = ""

            # Handle line with only whitespaces or comment.
            if not line.lstrip():
                if comment is None:
                    # Yield then reset pending comment on blank line
                    if pending_comment is not None:
                        yield pending_comment
                        pending_comment = None
                    if rules_set is None:
                        yield BlankLine()
                elif pending_comment is None:
                    pending_comment = comment
                else:
                    # Gather comments
                    pending_comment += comment
                continue

            # Rules set header
            if (rs := RulesSet.parse_header(line)) is not None:
                if rules_set is not None:
                    yield rules_set
                mlvo, layout_range, kccgst = rs
                rules_set = RulesSet(
                    mlvo=mlvo, layout_range=layout_range, kccgst=kccgst, rules=[]
                )
            # Group
            elif (group := Group.parse(line)) is not None:
                if rules_set is not None:
                    yield rules_set
                    rules_set = None
                if pending_comment is not None:
                    group.description = textwrap.dedent(pending_comment.text)
                yield group
            # Include
            elif (inc := Include.parse(line)) is not None:
                if rules_set is not None:
                    yield rules_set
                    rules_set = None
                yield inc
            # Rules
            elif rules_set is not None:
                if comment is not None:
                    if comment is not None:
                        # Current comment has priority
                        pending_comment = comment
                try:
                    rule = Rule.parse(
                        # mlvo=mlvo,
                        # layout_range=rules_set.layout_range,
                        # section=rules_set.section,
                        raw=line,
                    )
                except ValueError:
                    print(f"[ERROR] Invalid line {line_number}: {raw_line}")
                    raise
                # TODO use comment for rule?
                rules_set.rules.append(rule)
            # Unsupported: error
            else:
                line_ref = (
                    str(line_number)
                    if pending_line_number is None
                    else f"{pending_line_number}-{line_number}"
                )
                raise ValueError(f"Cannot parse line {line_ref}: {line}")
            pending_line_number = None
            pending_line = ""
            pending_comment = None
        if pending_line:
            raise ValueError(pending_line)
        if rules_set is not None:
            yield rules_set
        if pending_comment is not None:
            yield pending_comment

    @classmethod
    def serialize(
        cls,
        items: Iterable[Include | Group | RulesSet | Comment | BlankLine],
        version: Version,
        debug: bool = False,
    ) -> Iterable[str]:
        last: Include | Group | RulesSet | Comment | BlankLine = BlankLine()
        for i in items:
            if isinstance(i, BlankLine) and isinstance(last, BlankLine):
                # Merge blank lines
                continue
            elif isinstance(i, BlankLine) or isinstance(last, BlankLine):
                # Explicit blank line
                pass
            elif not (isinstance(i, Group) and isinstance(last, Group)):
                # No explicit blank line: add one to separate 2 entries, except
                # if they are both groups, so that related groups are kept together.
                yield "\n"
            last = i
            yield from i.serialize(version=version, debug=debug)

    @classmethod
    def render(
        cls, rules: Iterable[str], version: Version, debug: bool = False
    ) -> Iterable[str]:
        yield from cls.serialize(cls.parse(rules), version=version, debug=debug)
