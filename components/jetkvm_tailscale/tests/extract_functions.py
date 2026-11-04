"""Print functions of a production source, for a host test to include.

Usage: extract_functions.py <source> <name>...
"""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()


def function(name):
    match = re.search(r"^[A-Za-z_][^\n]*\b" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError("Missing production function: " + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


print("\n".join(function(name) for name in sys.argv[2:]))
