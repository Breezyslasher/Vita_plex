#!/usr/bin/env python3
"""
Writes the import stubs of VitaPlex's quick menu plugin (src/qm_plugin_psv/
paf_stubs.S): one for each PAF symbol the plugin's object file leaves
undefined, with the library and NID that ScePaf-RE gives it.

The NIDs come from a vitasdk YAML table, ScePaf.yml, made by
vitasdk-paf-component's emd2yml (libpaf/emd2yml) from ScePaf-RE's lib/*.emd,
at the commits scripts/vita/fetch_paf_headers.sh pins. The stubs are not weak:
a module importing a NID the firmware lacks fails to load, and VitaPlex then
falls back to ElevenMPV-A's plugin, rather than jumping through an empty stub.

Given the plugin's object built at -O0 as well as -O2, the stubs cover what
a compiler that inlines less than this one leaves out of line.

usage: gen_paf_stubs.py ScePaf.yml main.o [main-O0.o ...] > src/qm_plugin_psv/paf_stubs.S
       (arm-vita-eabi-nm must be on PATH)
"""

import re
import subprocess
import sys


def read_table(path):
    """{symbol: (kind, library, library_nid, nid)} from a vitasdk YAML table."""
    table = {}
    library = library_nid = kind = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^      (\w+):\s*$", line)
        if m:
            library, library_nid, kind = m.group(1), None, None
            continue
        m = re.match(r"^        nid: (0x[0-9A-Fa-f]{8})", line)
        if m:
            library_nid = int(m.group(1), 16)
            continue
        m = re.match(r"^        (functions|variables):\s*$", line)
        if m:
            kind = m.group(1)
            continue
        m = re.match(r"^          (\S+): (0x[0-9A-Fa-f]{8})", line)
        if m and kind:
            table[m.group(1)] = (kind, library, library_nid, int(m.group(2), 16))
    return table


def undefined(objs):
    syms = set()
    for obj in objs:
        out = subprocess.run(["arm-vita-eabi-nm", "-u", obj], check=True, capture_output=True,
                             text=True).stdout
        syms |= {line.split()[-1] for line in out.splitlines() if line.strip()}
    return sorted(syms)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    table = read_table(sys.argv[1])
    wanted = [s for s in undefined(sys.argv[2:]) if s in table]
    by_section = {}
    for sym in wanted:
        kind, library, library_nid, nid = table[sym]
        section = (".vitalink.fstubs." if kind == "functions" else ".vitalink.vstubs.") + library
        by_section.setdefault(section, []).append((sym, kind, library_nid, nid))

    print("/*")
    print(" * PAF imports of VitaPlex's quick menu plugin, written by")
    print(" * scripts/vita/gen_paf_stubs.py from ScePaf-RE's NIDs. Not weak, so a")
    print(" * firmware without one of them refuses to load the plugin.")
    print(" */")
    print()
    print("\t.arch armv7a")
    for section in sorted(by_section):
        # As vita-libs-gen-2 writes them.
        flags = '"ax",%progbits' if ".fstubs." in section else '"",%progbits'
        print()
        print(f"\t.section {section},{flags}")
        for sym, kind, library_nid, nid in by_section[section]:
            print()
            print("\t.align 4")
            print(f"\t.global {sym}")
            print(f"\t.type {sym}, {'%function' if kind == 'functions' else '%object'}")
            print(f"{sym}:")
            print("\t.word 0x00000000")
            print(f"\t.word 0x{library_nid:08X}")
            print(f"\t.word 0x{nid:08X}")
            print("\t.align 4")
    print()
    print('\t.section .note.GNU-stack,"",%progbits')


if __name__ == "__main__":
    main()
