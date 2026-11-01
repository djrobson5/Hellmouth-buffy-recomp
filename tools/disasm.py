#!/usr/bin/env python3
"""Prints a function's instructions from the generated code, with addresses named from final.map.

    disasm.py <name fragment or hex address> [--max N] [--map assets/final.map] [--gen generated/game]
"""
import argparse
import glob
import re

from buffymap import Map, short_name

_HEAD = re.compile(r"^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)")
_INSN = re.compile(r"^\t// ([0-9A-F]{8}) (.*)$")
_ADDR = re.compile(r"0x([0-9A-Fa-f]{5,8})\b")


def function_body(gen, va):
    want = f"X86_FUNC(__imp__sub_{va:08X})"
    for path in glob.glob(f"{gen}/x86_recomp.*.cpp"):
        with open(path, errors="replace") as f:
            found = False
            lines = []
            for line in f:
                if not found:
                    found = line.startswith(want)
                    continue
                if _HEAD.match(line):
                    break
                m = _INSN.match(line)
                if m:
                    lines.append((int(m[1], 16), m[2]))
            if found:
                return lines
    return None


def annotate(m, text):
    notes = []
    for a in _ADDR.findall(text):
        va = int(a, 16)
        s, off = m.nearest(va)
        if s and off < 0x4000:
            notes.append(short_name(s.name)[:60] + (f"+{off:#x}" if off else ""))
    return "  ; " + ", ".join(notes) if notes else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what")
    ap.add_argument("--max", type=int, default=120)
    ap.add_argument("--map", default="assets/final.map")
    ap.add_argument("--gen", default="generated/game")
    args = ap.parse_args()
    m = Map(args.map)
    if re.fullmatch(r"(0x)?[0-9A-Fa-f]{5,8}", args.what):
        targets = [int(args.what, 16)]
    else:
        targets = sorted({s.va for s in m.symbols if s.is_func and args.what in s.name})
        # hand-written assembly entries such as _acos aren't flagged as functions in the map
        targets = targets or sorted({s.va for s in m.symbols if s.section <= 9 and args.what in s.name})
        if not targets:
            raise SystemExit(f"nothing in the map matches {args.what}")
    if len(targets) > 6:
        for va in targets[:40]:
            print(f"{va:08X} {m.by_va[va].name}")
        raise SystemExit(f"{len(targets)} matches: be more specific")
    for va in targets:
        s = m.by_va.get(va)
        print(f"--- {va:08X} {s.name if s else '?'} ({s.lib + ':' + s.obj if s else ''})")
        body = function_body(args.gen, va)
        if body is None:
            print("not found in the generated code")
            continue
        for a, text in body[: args.max]:
            print(f"{a:08X}  {text}{annotate(m, text)}")
        if len(body) > args.max:
            print(f"... {len(body) - args.max} more")


if __name__ == "__main__":
    main()
