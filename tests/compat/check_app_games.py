#!/usr/bin/env python3
"""Check that the game lists of the app, the README and the docs agree.

1. Every game marked Working in "Games" in docs/COMPATIBILITY.md must be
   listed in app/src/GameList.cpp with the same variant, and the other way
   round. Untested and nonworking games are intentionally excluded from the
   app.
2. The tables "Multiplayer games", "N-Gage Arena games", "Other online
   games" and "Bridges" in README.md must have the same entries and status
   as the matching tables in docs/COMPATIBILITY.md. The README shows the
   status as a badge; its label ("Not%20Working") counts as the text
   ("Not working").
"""

import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

# README heading, COMPATIBILITY.md heading
SECTIONS = [
    ("### Multiplayer games", "## Games"),
    ("### N-Gage Arena games", "## N-Gage Arena games"),
    ("### Other online games", "## Other online games"),
    ("### Bridges", "## Bridges"),
]


def read(*path):
    with open(os.path.join(ROOT, *path), encoding="utf-8") as f:
        return f.read()


def table_rows(text, heading):
    """Rows of the first table after heading, as lists of cells."""
    lines = text.split("\n")
    try:
        start = lines.index(heading)
    except ValueError:
        return None
    rows = []
    for line in lines[start + 1:]:
        if line.startswith("#"):
            break
        if not line.startswith("|"):
            if rows:
                break
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows[1:]  # without the header row


def table_games():
    games = {}
    for cells in table_rows(read("docs", "COMPATIBILITY.md"), "## Games") or []:
        if len(cells) >= 5 and re.fullmatch(r"v\d+", cells[1]) and cells[4] == "Working":
            games[cells[0]] = int(cells[1][1:])
    return games


def app_games():
    games = {}
    source = re.sub(r"/\*.*?\*/", "", read("app", "src", "GameList.cpp"), flags=re.DOTALL)
    for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*(?:,\s*(?:"[^"]*"|NULL)\s*)?\}', source):
        games[m.group(1)] = int(m.group(2))
    return games


def check_app(errors):
    table, app = table_games(), app_games()
    for name, variant in sorted(table.items()):
        if name not in app:
            errors.append("missing in app: %s (v%d)" % (name, variant))
        elif app[name] != variant:
            errors.append("%s: table v%d, app v%d" % (name, variant, app[name]))
    for name in sorted(set(app) - set(table)):
        errors.append("not in table: %s" % name)
    return len(table)


def status(cell):
    """Plain status text of a table cell; a badge counts as its label."""
    m = re.fullmatch(r"!\[([^\]]*)\]\([^)]*\)", cell)
    if not m:
        return cell
    label = m.group(1).replace("%20", " ")
    return label[0] + label[1:].lower()


def check_readme(errors):
    readme, compat = read("README.md"), read("docs", "COMPATIBILITY.md")
    count = 0
    for rhead, chead in SECTIONS:
        r, c = table_rows(readme, rhead), table_rows(compat, chead)
        if r is None or c is None:
            errors.append("missing section: %s" % (rhead if r is None else chead))
            continue
        # first column = name, last column = status (a badge in the README)
        r = {row[0]: status(row[-1]) for row in r}
        c = {row[0]: row[-1] for row in c}
        for name in sorted(set(r) | set(c)):
            if name not in c:
                errors.append("%s: %s missing in COMPATIBILITY.md" % (chead, name))
            elif name not in r:
                errors.append("%s: %s missing in README.md" % (rhead, name))
            elif r[name] != c[name]:
                errors.append("%s: README %r, COMPATIBILITY.md %r" % (name, r[name], c[name]))
        count += len(r)
    return count


def main():
    errors = []
    games = check_app(errors)
    rows = check_readme(errors)
    for e in errors:
        print("ERROR:", e)
    print("%d app games and %d README rows checked, %d problems" % (games, rows, len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
