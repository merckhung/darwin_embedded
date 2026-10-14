#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""
Minimal and unsafe template engine based on `exec()`

Only a very limited subset of Python is allowed in the template.
However, this template engine is still considered unsafe, due to
the intrinsic unsafe nature of `exec()`. Nevertheless, this is deemed
sufficient because because both the template and the input data are
version controlled, and a successful attack would require malicious
content that would be conspicuous in any routine code review.
"""

from __future__ import annotations

import argparse
import ast
import re
import textwrap
from dataclasses import dataclass
from typing import Any, ClassVar, Iterable, Self


@dataclass
class Template:
    code: str

    INDENT_PATTERN: ClassVar[re.Pattern[str]] = re.compile(r"^( +)")
    KEYWORD_PATTERN: ClassVar[re.Pattern[str]] = re.compile(
        r"^\s*(end(?:if|for|while|with|def|class)|"
        r"if|elif|else|for|while|with|def|class)"
    )
    RENDER: ClassVar[str] = "render"
    SAFE_NODES: ClassVar[tuple[ast.AST, ...]] = (
        ast.Module,
        ast.If,
        ast.IfExp,
        ast.Expr,
        ast.Expression,
        ast.Constant,
        ast.Name,
        ast.Load,
        ast.UnaryOp,
        ast.BoolOp,
        ast.BinOp,
        ast.Add,
        ast.Sub,
        ast.Mult,
        ast.Div,
        ast.FloorDiv,
        ast.Mod,
        ast.Pow,
        ast.LShift,
        ast.RShift,
        ast.BitOr,
        ast.BitXor,
        ast.BitAnd,
        ast.MatMult,
        ast.Compare,
        ast.Eq,
        ast.NotEq,
        ast.Lt,
        ast.LtE,
        ast.Gt,
        ast.GtE,
        ast.Is,
        ast.IsNot,
        ast.In,
        ast.NotIn,
        ast.JoinedStr,
        ast.FormattedValue,
        ast.List,
        ast.Tuple,
        ast.Dict,
        ast.Attribute,
        ast.Yield,
        ast.For,
        ast.Subscript,
        ast.Call,
        ast.Or,
        ast.And,
        ast.Not,
        ast.Store,
        ast.Continue,
    )

    # Unused for now. Requires to add the functions explicitly in the "__builtins__" globals
    SAFE_FUNCTIONS: ClassVar[tuple[str, ...]] = ("range",)

    @classmethod
    def parse(cls, raw: Iterable[str]) -> Self:
        code = "\n".join(cls._parse(raw))

        # Guard allowed Python tokens as a minimal security

        try:
            _ast = ast.parse(code)
        except SyntaxError as exc:
            raise exc

        for node in ast.walk(_ast):
            if type(node) not in cls.SAFE_NODES:
                raise PermissionError(f"Unsupported node: {node}")
            elif isinstance(node, ast.Attribute):
                if node.attr.startswith("_"):
                    raise PermissionError(
                        f"Access to private attribute '{node.attr}' is forbidden."
                    )
            elif isinstance(node, ast.JoinedStr):
                # Guard against malicious values in the registry
                for v in node.values:
                    if not cls._is_simple(v):
                        raise PermissionError(f"Invalid simple f-string: {node}")
            elif isinstance(node, ast.Subscript):
                # Only allow `xxx[yyy]` where yyy is a constant that does not start with "_"
                match node.slice:
                    case ast.Constant(value=str(s)) if s.startswith("_"):
                        raise PermissionError(
                            f"Access to private subscript '{s}' is forbidden."
                        )
                    case ast.Constant():
                        pass  # int, non-private string: fine
                    case _:
                        raise PermissionError(f"Non-constant subscript is forbidden.")
            elif isinstance(node, ast.Call):
                # Only allow functions that are class methods
                # Only allow `.items()`
                if (
                    not isinstance(node.func, ast.Attribute)
                    or node.func.attr != "items"
                    or node.args
                    or node.keywords
                ) and (
                    not isinstance(node.func, ast.Name)
                    or node.func.id not in cls.SAFE_FUNCTIONS
                ):
                    raise PermissionError(
                        f"Access to function '{node.func}' is forbidden."
                    )

        code = f"def {cls.RENDER}():\n" + textwrap.indent(code, "    ")

        return cls(code)

    @classmethod
    def _parse(cls, raw: Iterable[str]) -> Iterable[str]:
        indent = ""
        for line in raw:
            if line.startswith("#"):
                line = line[2:]
                if not line:
                    continue
                if line.lstrip().startswith("#"):
                    # Skip comment
                    continue
                if m := cls.INDENT_PATTERN.match(line):
                    indent = m.group(1)
                else:
                    indent = ""
                if m := cls.KEYWORD_PATTERN.match(line):
                    if m.group(1).startswith("end"):
                        continue
                    indent += "    "
                yield line
            else:
                yield indent + f"yield f{repr(line)}"

    @classmethod
    def _is_simple(cls, node: ast.JoinedStr) -> bool:
        match node:
            case ast.Constant(value=value):
                return True
            case ast.FormattedValue(value=value, conversion=-1):
                return cls._is_simple_expr(value)
            case _:
                return False

    @classmethod
    def _is_simple_expr(cls, node: ast.AST) -> bool:
        """Only allow plain names and attribute access chains"""
        match node:
            case ast.Name():
                return not node.id.startswith("_")
            case ast.Attribute(value=inner, attr=attr_):
                return cls._is_simple_expr(inner) and not attr_.startswith("_")
            case _:
                return False

    def render(self, **kwargs) -> str:
        context: dict[str, Any] = dict(kwargs)
        context["__builtins__"] = {"range": range}
        exec(self.code, context)
        return "".join(context[self.RENDER]())


def run_render(args: argparse.Namespace):
    template = Template.parse(args.path)
    print(template.render())


def run_tests(args: argparse.Namespace):
    test_basic()


def test_basic():
    template = Template.parse(
        """\
# if x < 1:
    a
# elif x < 5:
    b
# else:
    c
# endif
""".splitlines()
    )

    output = template.render(x=3)
    assert output == "    b", output

    try:
        template = Template.parse(
            """
# [].__class__.__base__.__subclasses__()[140].__init__.__globals__['sys'].modules['os']
""".splitlines()
        )
    except PermissionError as e:
        ...
    else:
        raise Exception("Expection not raised")

    try:
        template = Template.parse(
            """
# (x for x in []).gi_frame.f_builtins['__import__']('os').system('whoami')
""".splitlines()
        )
    except PermissionError as e:
        ...
    else:
        raise Exception("Expection not raised")

    try:
        template = Template.parse("""! {x.__class__} = x""".splitlines())
    except PermissionError as e:
        ...
    else:
        raise Exception("Expection not raised")

    try:
        template = Template.parse("""{ { ().__class__ : "safe" } }""".splitlines())
    except PermissionError as e:
        ...
    else:
        raise Exception("Expection not raised")


if __name__ == "__main__":
    parser = argparse.ArgumentParser("Template rendering")
    subparsers = parser.add_subparsers()

    parser_render = subparsers.add_parser("render", help="Render template")
    parser_render.add_argument("path", type=argparse.FileType("rt", encoding="utf-8"))
    parser_render.set_defaults(run=run_render)

    parser_test = subparsers.add_parser("test", help="Run tests")
    parser_test.set_defaults(run=run_tests)

    args = parser.parse_args()
    args.run(args)
