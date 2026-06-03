#!/usr/bin/env python3
"""
Parenthesize return values: 'return X;' -> 'return (X);' (style(9)).

Line-oriented and conservative on purpose:

  * Only rewrites a line whose first non-whitespace token is 'return'
    followed by whitespace and an expression ending in ';' on the same
    line.  This never touches comments (they start with '*' or '/'),
    string contents, or 'return;'.
  * Skips a return whose value is *already* fully parenthesized
    (the first '(' matches the ')' just before ';'), so we never
    produce 'return ((x));'.
  * Skips macro-continuation lines (trailing backslash); the
    SM_ENOUGH_SPACE macro body is hand-formatted.
  * Multi-line return expressions (where ';' is on a later line) are
    left alone.

Trailing end-of-line comments are preserved.
"""
import re
import sys

RET = re.compile(r"^(?P<indent>[ \t]*)return[ \t]+(?P<expr>.*?);(?P<rest>[ \t]*(?:/\*.*)?)$")


def fully_parenthesized(expr):
    if not expr.startswith("("):
        return False
    depth = 0
    for i, c in enumerate(expr):
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                # The first '(' closes here; it wraps the whole expr
                # only if this is the final character.
                return i == len(expr) - 1
    return False


def process(text):
    out = []
    for line in text.split("\n"):
        if line.endswith("\\"):
            out.append(line)
            continue
        m = RET.match(line)
        if not m:
            out.append(line)
            continue
        expr = m.group("expr").strip()
        if expr == "" or fully_parenthesized(expr):
            out.append(line)
            continue
        out.append("%sreturn (%s);%s" % (m.group("indent"), expr, m.group("rest")))
    return "\n".join(out)


def main():
    for path in sys.argv[1:]:
        with open(path, encoding="utf-8") as f:
            text = f.read()
        with open(path, "w", encoding="utf-8") as f:
            f.write(process(text))


if __name__ == "__main__":
    main()
