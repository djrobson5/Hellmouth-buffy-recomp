#!/usr/bin/env python3
"""What the game calls in the XDK libraries, and whether the host replaces it with the same argument count.

A library function the host does not replace runs as recompiled library code, which is fine for pure logic and wrong
for anything that reaches for the hardware. This is a report to read, not a pass or fail: direct calls only, so calls
through a vtable or a function pointer are not counted.

    coverage.py [--map assets/final.map] [--gen generated/game] [--cfg config/game.toml] [--sdk sdk]
"""
import argparse
import collections
import glob
import re

from buffymap import Map, short_name

_HEAD = re.compile(r"^X86_FUNC\(__imp__sub_([0-9A-F]{8})\)")
_INSN = re.compile(r"^\t// ([0-9A-F]{8}) (.*)$")
_CALL = re.compile(r"^(call|jmp) 0x([0-9A-F]{8})$")
_RET = re.compile(r"^ret(?: 0x([0-9A-Fa-f]+))?$")
TEXT_END = 0x00591390


def host_decls(sdk):
    decls = {}
    for path in glob.glob(f"{sdk}/runtime/**/*", recursive=True):
        if not re.search(r"\.(cpp|h)$", path):
            continue
        text = open(path, errors="replace").read()
        for m in re.finditer(r"\bHLE_(STDCALL|FASTCALL)\(\s*(\w+)\s*,\s*(\d+)\s*\)", text):
            decls[m[2]] = (m[1], int(m[3]))
        for name in re.findall(r"X86_FUNC\(GUEST\((\w+)\)\)", text):
            decls.setdefault(name, ("custom", 0))
        # the families the sources define through a macro of their own
        for name, n in re.findall(r"^BUFFER\(\s*(\w+)\s*,\s*(\d+)", text, re.M):
            decls[f"IDirectSoundBuffer_{name}"] = ("STDCALL", int(n))
        for name, n in re.findall(r"^STREAM\(\s*(\w+)\s*,\s*(\d+)", text, re.M):
            decls[f"CDirectSoundStream_{name}"] = ("STDCALL", int(n))
        for name in re.findall(r"^COMPLEX_STATE\(\s*(\w+)", text, re.M):
            decls[f"D3DDevice_SetRenderState_{name}"] = ("STDCALL", 1)
        for name in re.findall(r"^CRT_MATH\(\s*(\w+)", text, re.M):
            decls[name] = ("custom", 0)
    return decls


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="assets/final.map")
    ap.add_argument("--gen", default="generated/game")
    ap.add_argument("--cfg", default="config/game.toml")
    ap.add_argument("--sdk", default="sdk")
    args = ap.parse_args()
    m = Map(args.map)

    block = re.search(r"^\[symbols\]\n(.*?)(?=^\[)", open(args.cfg).read(), re.M | re.S)[1]
    bound = {int(v, 16): k for k, v in re.findall(r"^(\w+) = 0x([0-9A-F]+)$", block, re.M)}
    decls = host_decls(args.sdk)

    calls = collections.Counter()
    rets = collections.defaultdict(set)
    for path in glob.glob(f"{args.gen}/x86_recomp.*.cpp"):
        cur = None
        with open(path, errors="replace") as f:
            for line in f:
                h = _HEAD.match(line)
                if h:
                    cur = int(h[1], 16)
                    continue
                i = _INSN.match(line)
                if not i or cur is None:
                    continue
                c = _CALL.match(i[2])
                if c and cur < TEXT_END:
                    target = int(c[2], 16)
                    if target >= TEXT_END:
                        calls[target] += 1
                r = _RET.match(i[2])
                if r and cur in bound:
                    rets[cur].add(int(r[1], 16) if r[1] else 0)

    print("== ABI: host argument count against the rets of the function it replaces")
    bad = 0
    for va, name in sorted(bound.items(), key=lambda x: x[1]):
        kind, n = decls.get(name, (None, 0))
        if kind not in ("STDCALL", "FASTCALL") or not rets[va]:
            continue
        if rets[va] != {n * 4}:
            bad += 1
            print(f"  MISMATCH {name}: host {kind} pops {n * 4}, guest rets {sorted(rets[va])}")
    print(f"  {bad} mismatches")

    print("\n== library functions game code calls directly, not replaced by the host")
    by_lib = collections.defaultdict(list)
    replaced = 0
    for target, count in calls.items():
        s = m.by_va.get(target)
        if not s:
            continue
        name = bound.get(target)
        if name and name in decls:
            replaced += 1
            continue
        by_lib[s.lib or s.obj].append((short_name(s.name), count, name))
    print(f"  {replaced} replaced by the host")
    for lib, items in sorted(by_lib.items()):
        print(f"  [{lib}] {len(items)}")
        print("    " + "  ".join(f"{n}({c})" for n, c, _ in sorted(items)))

    print("\n== bound in the config but with no host implementation")
    print("  " + " ".join(sorted(n for n in bound.values() if n not in decls)))


if __name__ == "__main__":
    main()
