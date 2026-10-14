# SPDX-License-Identifier: MIT

import dataclasses
import textwrap

from rules.generator.parser import (
    MLVO,
    BlankLine,
    Comment,
    Group,
    KcCGST,
    LayoutRange,
    Rule,
    RulesFile,
    RulesSet,
    Version,
)
from rules.generator.template import Template


def test_template():
    raw = textwrap.dedent("""\
    line 1
    # # This is a comment
    # # x = 2
    line 4
    line 5 x={x} {y:#04x} {{x}}

    # for i in (0, x - 1):
    #     for j in (1, 2):
    {j}. Item {i}
       …
    #     endfor
    ---
    # endfor
    ⁂
    """)

    template = Template.parse(raw.splitlines(keepends=True))
    print(template.code)

    expected = textwrap.dedent("""\
    line 1
    line 4
    line 5 x=2 0x7b {x}

    1. Item 0
       …
    2. Item 0
       …
    ---
    1. Item 1
       …
    2. Item 1
       …
    ---
    ⁂
    """)
    got = template.render(x=2, y=123)

    assert got == expected


def test_rule():
    rule = Rule(mlvo=("a", "b"), section=r"+%l[%i]%(v[%i]):%i")
    assert rule.to_numeric_index(0) == Rule(mlvo=("a", "b"), section=r"+%l%(v)")
    assert rule.to_numeric_index(1) == Rule(mlvo=("a", "b"), section=r"+%l[1]%(v[1]):1")
    assert rule.to_numeric_index(2) == Rule(mlvo=("a", "b"), section=r"+%l[2]%(v[2]):2")


def test_rules_set_numeric_index():
    rule = Rule(mlvo=("a", "b"), section=r"+%l[%i]%(v[%i]):%i")
    rules_set = RulesSet(
        mlvo=(MLVO.Layout, MLVO.Variant),
        layout_range=LayoutRange.Any,
        kccgst=KcCGST.Symbols,
        rules=[rule],
    )
    numeric = tuple(rules_set.to_numeric_indices(4))
    assert numeric == (
        dataclasses.replace(
            rules_set, layout_range=0, rules=[Rule(mlvo=("a", "b"), section=r"+%l%(v)")]
        ),
        dataclasses.replace(
            rules_set,
            layout_range=1,
            rules=[Rule(mlvo=("a", "b"), section=r"+%l[1]%(v[1]):1")],
        ),
        dataclasses.replace(
            rules_set,
            layout_range=2,
            rules=[Rule(mlvo=("a", "b"), section=r"+%l[2]%(v[2]):2")],
        ),
        dataclasses.replace(
            rules_set,
            layout_range=3,
            rules=[Rule(mlvo=("a", "b"), section=r"+%l[3]%(v[3]):3")],
        ),
        dataclasses.replace(
            rules_set,
            layout_range=4,
            rules=[Rule(mlvo=("a", "b"), section=r"+%l[4]%(v[4]):4")],
        ),
    )


def test_rules_set_serialize():
    rule1 = Rule(mlvo=("a", "bbbbbbbbbbbbbbbbbbb"), section=r"+%l[%i]%(v[%i]):%i")
    rule2 = Rule(mlvo=("$aaaaaaaaaaaaaa", "$b"), section=r"+%l[%i]%(v[%i]):%i")
    rules_set = RulesSet(
        mlvo=(MLVO.Layout, MLVO.Variant),
        layout_range=LayoutRange.Any,
        kccgst=KcCGST.Symbols,
        rules=[rule1, rule2],
    )

    # V2
    expected = textwrap.dedent("""\
        ! layout		 variant		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l%(v)
         $aaaaaaaaaaaaaa	$b			=	+%l%(v)

        ! layout[1]		 variant[1]		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l[1]%(v[1]):1
         $aaaaaaaaaaaaaa	$b			=	+%l[1]%(v[1]):1

        ! layout[2]		 variant[2]		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l[2]%(v[2]):2
         $aaaaaaaaaaaaaa	$b			=	+%l[2]%(v[2]):2

        ! layout[3]		 variant[3]		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l[3]%(v[3]):3
         $aaaaaaaaaaaaaa	$b			=	+%l[3]%(v[3]):3

        ! layout[4]		 variant[4]		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l[4]%(v[4]):4
         $aaaaaaaaaaaaaa	$b			=	+%l[4]%(v[4]):4
    """)
    got = "".join(rules_set.serialize(Version.V2))
    assert got == expected

    # V3
    expected = textwrap.dedent("""\
        ! layout[any]		 variant[any]		=	symbols
          a			 bbbbbbbbbbbbbbbbbbb	=	+%l[%i]%(v[%i]):%i
         $aaaaaaaaaaaaaa	$b			=	+%l[%i]%(v[%i]):%i
    """)
    got = "".join(rules_set.serialize(Version.V3))
    assert got == expected


def test_group():
    raw = "! $g = 11 2 4 1 3 "
    g = Group(name="$g", members={"1", "11", "2", "3", "4"})
    assert Group.parse(raw) == g
    assert "".join(g.serialize()) == "! $g = 1 2 3 4 11\n"


def test_rules_file():
    raw = textwrap.dedent("""\
    //
    // Some comment
    //

    // Group comment
    // on 2 lines
    ! $g1 = e1 e2 \\
            e3 // comment
    ! model layout[first] \
       = symbols

      *       a         = A/a:%i
      * * =             %l[%i]%(v[%i]):%i
    """)

    expected = (
        Comment(text="\n Some comment\n"),
        BlankLine(),
        Group(
            name="$g1",
            members={"e1", "e2", "e3"},
            component=None,
            description="Group comment\non 2 lines",
        ),
        RulesSet(
            mlvo=(MLVO.Model, MLVO.Layout),
            layout_range=LayoutRange.First,
            kccgst=KcCGST.Symbols,
            rules=[
                Rule(mlvo=("*", "a"), section="A/a:%i"),
                Rule(mlvo=("*", "*"), section="%l[%i]%(v[%i]):%i"),
            ],
        ),
    )
    got = tuple(RulesFile.parse(raw.splitlines(keepends=True)))
    assert got == expected

    expected = textwrap.dedent("""\
        //
        // Some comment
        //

        // Group comment
        // on 2 lines
        ! $g1 = e1 e2 e3

        ! model	layout[first]	=	symbols
          *	a		=	A/a:%i
          *	*		=	%l[%i]%(v[%i]):%i
    """)
    got = "".join(RulesFile.serialize(got, version=Version.V3))
    assert got == expected
