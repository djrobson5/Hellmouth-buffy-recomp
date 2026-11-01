#!/usr/bin/env python3
"""Checks every layout key and data address the runtime requires is in the config, so none aborts at run time.

    check_layout.py [--cfg config/game.toml] [--layout sdk/xdk/4432/layout.toml] [--sdk sdk]
"""
import argparse
import glob
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cfg", default="config/game.toml")
    ap.add_argument("--layout", default="sdk/xdk/4432/layout.toml")
    ap.add_argument("--sdk", default="sdk")
    args = ap.parse_args()
    keys, data = set(), set()
    for path in glob.glob(f"{args.sdk}/runtime/**/*", recursive=True):
        if re.search(r"\.(cpp|h)$", path):
            text = open(path, errors="replace").read()
            keys |= set(re.findall(r"\bXDK_KEY\((\w+)\)", text)) - {"name"}
            # a key used directly falls back to "unknown" if the config lacks it, with no XDK_REQUIRE to report it
            keys |= set(re.findall(r"\bXDK_(\w+)", text)) - {"KEY", "DATA", "REQUIRE"}
            data |= set(re.findall(r"\bXDK_DATA\((\w+)\)", text)) - {"name"}
    have_keys = set(re.findall(r"^(\w+)\s*=", open(args.layout).read(), re.M))
    block = re.search(r"^\[data\]\n(.*)", open(args.cfg).read(), re.M | re.S)[1]
    have_data = {k for k, v in re.findall(r"^(\w+) = (0x[0-9A-F]+)$", block, re.M) if int(v, 16)}
    missing_keys, missing_data = sorted(keys - have_keys), sorted(data - have_data)
    print(f"layout keys required {len(keys)}, missing {missing_keys or 'none'}")
    print(f"data addresses required {len(data)}, missing {missing_data or 'none'}")
    print(f"layout keys given but unused: {sorted(have_keys - keys) or 'none'}")
    raise SystemExit(1 if missing_keys or missing_data else 0)


if __name__ == "__main__":
    main()
