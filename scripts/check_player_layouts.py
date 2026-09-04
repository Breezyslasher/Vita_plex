#!/usr/bin/env python3
"""Check every player layout XML against the contract player_activity.cpp assumes.

Three crashes have come out of a layout disagreeing with that contract, each one
only reachable on a device and none of them a compile error:

  * a bound id missing from a layout -- BRLS_BIND resolves lazily and throws
    ViewNotFoundException the first time that view is touched;
  * a button without focusable="true" -- setCustomNavigationRoute calls fatal()
    on a view that cannot take focus, which aborts the process;
  * an image pointing at a file that is not in the tree -- romfs::get throws
    std::invalid_argument at the first draw.

A fourth kind does not crash, which makes it worse: it looks perfect and simply
does not respond.

  * an anonymous full-size or growing box declared after the controls --
    borealis hit-tests siblings in document order, later on top, so it takes
    their taps while drawing nothing, and with no id nothing can hide it.

All of them are decidable by reading the sources, so decide them here instead of
on a phone. Run with no arguments from the repo root; exits non-zero on any
finding.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include/activity/player_activity.hpp"
RESOURCES = ROOT / "resources"
# The classic layout is the reference: it is the one every platform has always
# used, so where a layout disagrees with it, the layout is what is wrong.
REFERENCE = RESOURCES / "xml/activity/player.xml"
LAYOUTS = sorted((RESOURCES / "xml/activity").glob("player*.xml"))

ELEMENT = re.compile(r"<((?:\w+:)?\w+)((?:[^<>])*?)/?>", re.S)
# Opening, closing and self-closing tags alike, for walking the tree by depth.
ANY_TAG = re.compile(r"<(/?)((?:\w+:)?[\w?!-]+)((?:[^<>])*?)(/?)>", re.S)


def elements(path):
    """id -> (tag, attribute text) for every element in a layout carrying an id."""
    out = {}
    for m in ELEMENT.finditer(path.read_text()):
        tag, attrs = m.group(1), m.group(2)
        found = re.search(r'\bid="([^"]+)"', attrs)
        if found:
            out[found.group(1)] = (tag, attrs)
    return out


def burying_siblings(path):
    """Top-level boxes declared after the controls that can silently eat their taps.

    borealis draws AND hit-tests siblings in document order, later on top, so a
    box declared after player/controls covers it wherever the two overlap --
    even with no background, drawing nothing at all. That has now cost two
    rounds of "the buttons don't work": first player/center_controls, absolute
    at 100% x 100%, and then a bare flex spacer that grows to 41% of the screen.

    A layout cannot say which of these are wanted, since the answer differs per
    mode and is decided in code. What it can require is that such a box carry an
    id, because an id is the only way the activity can hide it -- the spacer had
    none, which is exactly why it was invisible to everything but a finger.
    """
    src = path.read_text()
    depth, seen_controls, out = 0, False, []
    for m in ANY_TAG.finditer(src):
        closing, tag, attrs, selfclosing = m.groups()
        if tag.startswith("?") or tag.startswith("!"):
            continue
        if closing:
            depth -= 1
            continue
        if depth == 1:
            vid = re.search(r'\bid="([^"]+)"', attrs)
            if vid and vid.group(1) in ("player/controls", "player/center_controls"):
                seen_controls = True
            elif seen_controls and not vid:
                grow = re.search(r'\bgrow="([^"]+)"', attrs)
                covers = 'height="100%"' in attrs or (grow and float(grow.group(1)) > 0)
                if covers and 'visibility="gone"' not in attrs:
                    out.append(src[: m.start()].count("\n") + 1)
        if not selfclosing:
            depth += 1
    return out


def bound_ids():
    """The ids player_activity.hpp binds, which every layout must therefore declare."""
    return {
        m.group(3): m.group(1)
        for m in re.finditer(
            r'BRLS_BIND\(\s*([\w:]+)\s*,\s*(\w+)\s*,\s*"([^"]+)"', HEADER.read_text()
        )
    }


def main():
    problems = []
    binds = bound_ids()
    reference = elements(REFERENCE)

    for layout in LAYOUTS:
        name = layout.relative_to(ROOT)
        found = elements(layout)

        for vid in sorted(binds):
            if vid not in found:
                problems.append(f"{name}: no view with id {vid!r} (BRLS_BIND throws on it)")

        for vid, (tag, attrs) in sorted(found.items()):
            if vid not in reference:
                continue
            ref_tag, ref_attrs = reference[vid]
            if tag != ref_tag:
                problems.append(f"{name}: {vid} is <{tag}>, classic has <{ref_tag}>")
            focusable = 'focusable="true"' in attrs
            if 'focusable="true"' in ref_attrs and not focusable:
                problems.append(
                    f"{name}: {vid} is missing focusable=\"true\" that classic has "
                    f"(setCustomNavigationRoute aborts on it)"
                )

        for ref in sorted(set(re.findall(r'@res/([^"]+)', layout.read_text()))):
            if not (RESOURCES / ref).is_file():
                problems.append(f"{name}: references @res/{ref}, which is not in the tree")

        for line in burying_siblings(layout):
            problems.append(
                f"{name}:{line}: a growing/full-height box with no id sits after the "
                f"controls — it draws nothing but still takes their taps, and with no "
                f"id nothing can hide it"
            )

    # The same throw reaches code-set images, so hold those to the same rule.
    for source in sorted((ROOT / "src").rglob("*.cpp")):
        for ref in re.findall(r'setImageFromRes\(\s*"([^"]+\.\w+)"', source.read_text()):
            if not (RESOURCES / ref).is_file():
                problems.append(f"{source.relative_to(ROOT)}: setImageFromRes({ref!r}) has no such file")

    if problems:
        print(f"{len(problems)} problem(s):")
        for p in problems:
            print(f"  {p}")
        return 1

    print(f"ok: {len(LAYOUTS)} layout(s), {len(binds)} bound ids, all resources present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
