# SPDX-License-Identifier: MIT

from __future__ import annotations

import sys
import xml.etree.ElementTree as ET
from collections.abc import Iterable
from dataclasses import dataclass
from enum import unique
from pathlib import Path
from typing import Self

from .parser import Group


@dataclass(frozen=True)
class Registry:
    path: Path
    groups: dict[str, Group]
    options: tuple[str]

    @classmethod
    def load(cls, path: Path) -> Self:
        options = tuple(cls.load_options(path))

        return cls(
            path=path,
            groups={},  # TODO
            options=options,
        )

    @classmethod
    def load_options(cls, path: Path) -> Iterable[str]:
        """
        Yields all Options from the given XML file
        """
        tree = ET.parse(path)
        root = tree.getroot()

        for option in root.iter("option"):
            yield cls.fetch_name(option)

    @classmethod
    def fetch_subelement(cls, parent, name):
        sub_element = parent.findall(name)
        if sub_element is not None and len(sub_element) == 1:
            return sub_element[0]
        return None

    @classmethod
    def fetch_text(cls, parent, name) -> str | None:
        sub_element = cls.fetch_subelement(parent, name)
        if sub_element is None:
            return None
        return sub_element.text

    @classmethod
    def fetch_name(cls, elem) -> str:
        try:
            ci_element = (
                elem
                if elem.tag == "configItem"
                else cls.fetch_subelement(elem, "configItem")
            )
            name = cls.fetch_text(ci_element, "name")
            assert name is not None
            return name
        except AssertionError as e:
            endl = "\n"  # f{} cannot contain backslashes
            e.args = (
                f"\nFor element {ET.tostring(elem).decode('utf-8')}\n{endl.join(e.args)}",
            )
            raise e
