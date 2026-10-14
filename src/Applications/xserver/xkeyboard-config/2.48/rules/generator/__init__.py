# SPDX-License-Identifier: MIT

import itertools
import re
import string
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, TextIO

from .compatibility import ROOT, RULES, SYMBOLS, CompatMappings, CompatSymbolsMapping
from .options import Section
from .parser import RulesFile, Version
from .registry import Registry
from .template import Template


def generate_rules(
    template_content: Iterable[str],
    version: Version,
    ruleset: str,
    compat: bool,
    debug: bool,
) -> Iterable[str]:
    """
    Generate rules file from template
    """
    template = Template.parse(template_content)
    # “base” is used as a template for all rulesets.
    registry_files = (RULES / "base.xml", RULES / "base.extras.xml")
    registries = tuple(map(Registry.load, registry_files))

    # Options: render template with empty parameters and detect static options
    rules = template.render(
        groups=(),
        ruleset=ruleset,
        compat=compat,
        layouts_compat_mappings=[],
        variants_compat_mappings=[],
        options=defaultdict(tuple),
    )
    options = {
        s: tuple(s.get_options(xkb_root=ROOT, registries=registries, rules=rules))
        for s in Section
    }

    groups = tuple(itertools.chain.from_iterable(r.groups.values() for r in registries))

    # Compat mappings (aliases)
    compat_mappings = CompatMappings.load(
        layouts_path=RULES / "compat" / "layoutsMapping.lst" if compat else None,
        variants_path=RULES / "compat" / "variantsMapping.lst" if compat else None,
        vendors_path=RULES / "compat" / "variantsMapping-vendors.lst",
        skip_if_source_file_exists=True,
    )

    rules = template.render(
        groups=groups,
        ruleset=ruleset,
        compat=compat,
        layouts_compat_mappings=compat_mappings.layouts,
        variants_compat_mappings=compat_mappings.variants + compat_mappings.vendors,
        options=options,
    )
    return RulesFile.render(rules.splitlines(), version=version, debug=debug)


@dataclass
class SymbolsFile:
    path: Path
    content: str


SYMBOLS_TEMPLATE = string.Template("""
// Compatibility mapping
partial xkb_symbols "${alias}" {
    include "${target}"
};
""")


def generate_symbols(destination: Path) -> Iterable[SymbolsFile]:
    """
    Append xkb_symbols compat entries
    """
    mappings = CompatMappings.load(
        variants_path=RULES / "compat" / "variantsMapping.lst",
        skip_if_source_file_exists=False,
    )

    # Group by alias symbol file
    files = defaultdict(list)
    for mapping in mappings.variants:
        files[mapping.source.layout].append(mapping)

    for filename, mappings in files.items():
        src_path: Path = SYMBOLS / filename
        # Get original file content
        content = src_path.read_text(encoding="utf-8")
        # Check that there is no clash with existing sections
        new_sections = "|".join(re.escape(m.source.variant) for m in mappings)
        pattern = re.compile(rf'xkb_symbols\s+"(?P<section>{new_sections})"')
        for line_number, line in enumerate(content.splitlines(), start=1):
            # Drop comments
            line = line.split("//")[0]
            # Check for clashing section definition
            if m := pattern.search(line):
                l1 = mappings[0].source
                section = m.group("section")
                raise ValueError(
                    f'Cannot add compatibility section in symbols/{l1.layout}: "{section}" already exists at line {line_number}'
                )
        # Add compat sections
        for m in mappings:
            content += SYMBOLS_TEMPLATE.substitute(
                alias=m.source.variant, target=m.destination
            )

        yield SymbolsFile(path=destination / filename, content=content)
