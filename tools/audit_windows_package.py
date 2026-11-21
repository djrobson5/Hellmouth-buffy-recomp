#!/usr/bin/env python3
"""Audits the Windows release folder (and its zip): it must hold no file made from the player's game, and nothing that names the
machine it was built on. Run by tools/package_windows.sh; fails (exit 1) on the first kind of problem it finds, after listing all.

    audit_windows_package.py <folder> --dll <game_code/Buffy.dll> --xbe <assets/Default.xbe> --generated <generated/game>
                             --abi-inputs <game_code/abi_inputs.txt> [--zip <file>] [--version <x.y.z>]

What it checks:
  1. the files are exactly the allowed ones (a list below): nothing else, and none missing; every file is listed with its size
  2. none has a name of a game derived file (generated code, config/game.toml, assets, the DLL, image_symbols.h, ...)
  3. none holds the game's code or data: 64 windows of bytes from the code of the DLL the build made, 64 from the game's xbe and 64
     lines of the generated C++ are looked for in every file (a copy of any of them, whole or in part, would hold some)
  4. the headers that ship are the ones the ABI token was computed from (abi_inputs.txt), and the executable's stamp is the DLL's
  5. none names the machine or the user that built it (/Users/<name>/, the home folder, the user name)
  6. the zip holds the folder's files and nothing else, with no links
"""
import argparse
import getpass
import hashlib
import mmap
import os
import re
import struct
import sys
import zipfile

# every file the release holds: the exe, the compiler folder, the licences and the player's README
ALLOWED = [
    r"Buffy\.exe", r"README\.txt", r"LICENSE", r"THIRD_PARTY\.txt",
    r"licenses/[A-Za-z0-9_.+-]+",
    r"compiler/(clang|lld-link|xbe_recomp|buffy_config)\.exe",
    r"compiler/(libLLVM-23|libclang-cpp|libc\+\+|libunwind)\.dll",
    r"compiler/(sdk_names\.txt|Buffy\.lib|LLVM-LICENSE\.txt)",
    r"compiler/include/cpu/(x86_ops\.h|x86_context\.h)",
    r"compiler/include/freestanding/(cmath|cstdint|cstring|limits|type_traits)",
    r"compiler/xdk/4432/layout\.toml",
]
# files that have to be there
REQUIRED = [
    "Buffy.exe", "README.txt", "LICENSE", "THIRD_PARTY.txt", "licenses/LLVM-LICENSE.txt",
    "compiler/clang.exe", "compiler/lld-link.exe", "compiler/xbe_recomp.exe", "compiler/buffy_config.exe",
    "compiler/libLLVM-23.dll", "compiler/libclang-cpp.dll", "compiler/libc++.dll", "compiler/libunwind.dll",
    "compiler/sdk_names.txt", "compiler/Buffy.lib", "compiler/LLVM-LICENSE.txt",
    "compiler/include/cpu/x86_ops.h", "compiler/include/cpu/x86_context.h",
    "compiler/include/freestanding/cmath", "compiler/include/freestanding/cstdint", "compiler/include/freestanding/cstring",
    "compiler/include/freestanding/limits", "compiler/include/freestanding/type_traits",
    "compiler/xdk/4432/layout.toml",
]
# names of what is made from the game, wherever they are
FORBIDDEN_NAMES = re.compile(
    r"(\.xbe|\.iso|\.xpr|\.pak|\.wma|\.wmv|\.obj|\.o)$|^(final\.map|game\.toml|image_symbols\.h|exports\.def|imports\.def|code\.lib|Buffy\.dll"
    r"|x86_recomp.*|x86_func_mapping.*|disc_image\.txt|abi_inputs\.txt|game_code_flags\.h)$", re.I)
STAMP = re.compile(rb"xbe-game-code abi=[0-9a-z]+ image=[0-9a-f]+ symbols=[0-9a-f]+")


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def text_section(path):
    """The raw bytes of a PE file's .text: where its code is."""
    with open(path, "rb") as f:
        data = f.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    at = pe + 24 + opt_size
    for i in range(count):
        name = data[at + i * 40:at + i * 40 + 8].rstrip(b"\0")
        size, raw = struct.unpack_from("<II", data, at + i * 40 + 16)
        if name == b".text":
            return data[raw:raw + size]
    raise SystemExit("no .text in " + path)


def windows(data, count, size, start=0):
    """count windows of size bytes taken evenly from data, skipping any that are mostly one value (padding, zeros)."""
    out = []
    span = max(1, (len(data) - start - size) // (count * 2))
    pos = start
    while len(out) < count and pos + size <= len(data):
        w = bytes(data[pos:pos + size])
        if len(set(w)) >= 12:
            out.append(w)
            pos += span
        else:
            pos += size
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--dll", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--generated", required=True)
    ap.add_argument("--abi-inputs", required=True)
    ap.add_argument("--zip")
    ap.add_argument("--version")
    a = ap.parse_args()
    folder = os.path.abspath(a.folder)
    problems = []

    # 1. the files
    files = []
    for root, dirs, names in os.walk(folder):
        for n in names:
            p = os.path.join(root, n)
            if os.path.islink(p):
                problems.append("a link: " + os.path.relpath(p, folder))
            files.append(os.path.relpath(p, folder).replace(os.sep, "/"))
    files.sort()
    print("files in %s:" % folder)
    total = 0
    for f in files:
        size = os.path.getsize(os.path.join(folder, f))
        total += size
        print("  %12d  %s" % (size, f))
    print("  %12d  total, %d files" % (total, len(files)))
    allowed = re.compile("^(?:" + "|".join(ALLOWED) + ")$")
    for f in files:
        if not allowed.match(f):
            problems.append("not an allowed file: " + f)
    for f in REQUIRED:
        if f not in files:
            problems.append("missing: " + f)
    # 2. names
    for f in files:
        if FORBIDDEN_NAMES.search(os.path.basename(f)):
            problems.append("a name of something made from the game: " + f)

    # 3. the game's code and data, looked for in every file
    dll_text = text_section(a.dll)
    with open(a.xbe, "rb") as f:
        xbe = f.read()
    probes = [("code of Buffy.dll", w) for w in windows(dll_text, 64, 48)]
    probes += [("Default.xbe", w) for w in windows(xbe, 64, 48, start=0x1000)]
    lines = []
    for n in sorted(os.listdir(a.generated)):
        if n.startswith("x86_recomp.") and n.endswith(".cpp") and len(lines) < 64:
            with open(os.path.join(a.generated, n), "rb") as f:
                body = [l.strip() for l in f.read().split(b"\n")]
            # a line with an address of the game in it: the tools' own templates are no copy of the game
            body = [l for l in body if len(l) >= 40 and re.search(rb"(sub_|0x)[0-9A-Fa-f]{6,8}", l)]
            if body:
                lines.append(body[len(body) // 2])
    probes += [("generated C++", l) for l in lines]
    print("looking for %d pieces of the game's code and data (%d + %d byte windows of 48 bytes, %d lines) in %d files" %
          (len(probes), 64, 64, len(lines), len(files)))
    for f in files:
        p = os.path.join(folder, f)
        if os.path.getsize(p) == 0:
            continue
        with open(p, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
            for what, w in probes:
                at = m.find(w)
                if at >= 0:
                    problems.append("%s holds a piece of %s (at byte %d)" % (f, what, at))
                    break
            # 5. who built it
            for pat in (rb"/Users/[^/\0\s]+/", rb"[A-Za-z]:\\Users\\", re.escape(os.path.expanduser("~").encode()),
                        re.escape(getpass.getuser().encode()) if len(getpass.getuser()) >= 4 else None):
                if pat is None:
                    continue
                hit = re.search(pat, m)
                if hit:
                    problems.append("%s names the machine or user that built it: %r" % (f, bytes(hit.group(0))[:60]))
                    break

    # 4. the headers are the ones the ABI token covers; the stamp is the DLL's
    hashes = {}
    for line in open(a.abi_inputs, encoding="utf-8"):
        m = re.match(r"^([0-9a-f]{40}) (\S+)$", line.strip())
        if m:
            hashes[m.group(2)] = m.group(1)
    checked = 0
    for rel in files:
        if rel.startswith("compiler/include/"):
            key = "sdk/runtime/cpu/" + rel[len("compiler/include/"):].replace("cpu/", "", 1)
            want = hashes.get(key)
            if want is None:
                problems.append("%s is not among the ABI token's inputs (%s)" % (rel, key))
            elif sha1(os.path.join(folder, rel)) != want:
                problems.append("%s differs from the file the ABI token was computed from" % rel)
            else:
                checked += 1
    print("%d headers equal the ones in the ABI token's inputs" % checked)
    with open(os.path.join(folder, "Buffy.exe"), "rb") as f:
        exe_stamps = set(STAMP.findall(f.read()))
    with open(a.dll, "rb") as f:
        dll_stamps = set(STAMP.findall(f.read()))
    print("Buffy.exe stamp: %s" % sorted(s.decode() for s in exe_stamps))
    print("Buffy.dll stamp: %s" % sorted(s.decode() for s in dll_stamps))
    if len(exe_stamps) != 1 or exe_stamps != dll_stamps:
        problems.append("the executable's stamp is not the DLL's")

    # 6. the zip
    if a.zip:
        with zipfile.ZipFile(a.zip) as z:
            top = None
            names = []
            for info in z.infolist():
                if info.is_dir():
                    continue
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    problems.append("a link in the zip: " + info.filename)
                parts = info.filename.split("/", 1)
                top = top or parts[0]
                if parts[0] != top or len(parts) < 2:
                    problems.append("outside the zip's one folder: " + info.filename)
                names.append(parts[1] if len(parts) > 1 else info.filename)
            if sorted(names) != files:
                problems.append("the zip's files are not the folder's: " + ", ".join(sorted(set(names) ^ set(files))[:5]))
            print("zip: %s, %d bytes, %d files in %s/" % (a.zip, os.path.getsize(a.zip), len(names), top))
            if a.version and top and a.version not in top:
                problems.append("the zip's folder %s does not carry the version %s" % (top, a.version))

    if problems:
        print("\nAUDIT FAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nAUDIT PASSED: only the allowed files, none made from the game, none naming the build machine")
    return 0


if __name__ == "__main__":
    sys.exit(main())
