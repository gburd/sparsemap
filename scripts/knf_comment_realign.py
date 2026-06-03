#!/usr/bin/env python3
"""
Realign C block-comment continuation lines after a tab-based reindent.

clang-format (with ReflowComments: false) re-indents the opening "/*"
line of a block comment but leaves the " *" continuation lines at their
original column, which misaligns every indented comment when switching
to 8-column tabs.  This pass walks the file with a real C tokenizer
state machine (tracking strings, char literals, // and /* comments) and,
for every line that *starts* inside a block comment and whose first
non-whitespace character is '*', rewrites its leading whitespace to
match the indentation of the line that opened the comment, plus one
space.  Content after the '*' is never touched, so ASCII tables and
bit-diagrams inside comments keep their internal alignment.

Only whitespace inside comment interiors is changed; code is untouched.
"""
import sys


def leading_ws(line):
    i = 0
    while i < len(line) and line[i] in " \t":
        i += 1
    return line[:i]


def process(text):
    lines = text.split("\n")
    out = []

    # State carried across lines.
    in_block = False        # inside /* ... */
    block_open_indent = ""  # leading ws of the line that opened it

    for line in lines:
        starts_in_block = in_block
        open_indent_for_line = block_open_indent

        # Scan this line to update state for the *next* line, and to
        # learn whether/where a block comment opened on this line.
        i = 0
        n = len(line)
        in_string = False
        in_char = False
        line_opened_block_indent = None
        while i < n:
            c = line[i]
            nxt = line[i + 1] if i + 1 < n else ""
            if in_block:
                if c == "*" and nxt == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if in_string:
                if c == "\\":
                    i += 2
                    continue
                if c == '"':
                    in_string = False
                i += 1
                continue
            if in_char:
                if c == "\\":
                    i += 2
                    continue
                if c == "'":
                    in_char = False
                i += 1
                continue
            # normal code
            if c == "/" and nxt == "/":
                break  # rest of line is a line comment
            if c == "/" and nxt == "*":
                in_block = True
                block_open_indent = leading_ws(line)
                if line_opened_block_indent is None:
                    line_opened_block_indent = block_open_indent
                i += 2
                continue
            if c == '"':
                in_string = True
                i += 1
                continue
            if c == "'":
                in_char = True
                i += 1
                continue
            i += 1

        # Rewrite continuation lines: those that began inside a block
        # comment and whose first non-ws char is '*'.
        stripped = line.lstrip(" \t")
        if starts_in_block and stripped.startswith("*"):
            out.append(open_indent_for_line + " " + stripped)
        else:
            out.append(line)

    return "\n".join(out)


def main():
    for path in sys.argv[1:]:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
        new = process(text)
        with open(path, "w", encoding="utf-8") as f:
            f.write(new)


if __name__ == "__main__":
    main()
