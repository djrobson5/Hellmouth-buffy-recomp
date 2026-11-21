#!/usr/bin/env python3
"""Audits the Mac release folder (and its zip): it must hold no file made from the player's game, and nothing that names the machine it
was built on. Run by tools/package_mac.sh (on the folder, and on the zip's contents extracted); fails (exit 1) after listing every problem.

    audit_mac_package.py <folder> --dylib <build/Buffy.dylib> --xbe <assets/Default.xbe> --generated <generated/game>
                         --abi-inputs <game_code/abi_inputs.txt> --version <x.y.z> [--zip <file>] [--repo <this repo>]

What it checks (the Mac counterpart of tools/audit_windows_package.py):
  1. the files are exactly the allowed ones (a list below): nothing else, and none missing; every file is listed with its size; the only
     links are inside Buffy.app (there are none)
  2. none has a name of a game derived file (generated code, config/game.toml, assets, the game's dylib, image_symbols.h, the achievement
     pictures and icon the first start makes from the disc: achievements/, Buffy.png, Buffy.ico, Buffy.icns, ...)
  3. none holds the game's code or data: 64 windows of bytes from the code of the dylib the build made, 64 from the game's xbe and 64 lines
     of the generated C++ are looked for in every file
  4. the headers that ship are the ones the ABI token was computed from (abi_inputs.txt), and the executable's stamp is the dylib's
  5. none names the machine or the user that built it (/Users/<name>/, the home folder, the user name, the e-mail's name, the host's name)
  6. Info.plist, recomplauncher.json and Resources/settings.json agree with each other and with the release (version, bundle ID, the
     program RecompLauncher starts, the macOS it needs)
  7. every Mach-O is one of the app's own (the executable, two libraries, four programs) and tools/mac/check_mac_deps.sh passes on the app:
     arm64, macOS 13.0 or earlier, only system and shipped libraries, signed; and the app verifies (codesign --verify --deep --strict), with
     an ad hoc signature and no hardened runtime
  8. the zip (ditto's) holds the folder's files and nothing else (no __MACOSX, no ._ files, no links outside the app), with the
     executables' permissions
"""
import argparse
import getpass
import hashlib
import json
import mmap
import os
import plistlib
import re
import socket
import struct
import subprocess
import sys
import zipfile

APP = "Buffy.app"
NAME = "[A-Za-z0-9_.+-]+"
# every file the release holds
ALLOWED = [
    r"README\.txt", r"recomplauncher\.json",
    APP + r"/Contents/Info\.plist", APP + r"/Contents/_CodeSignature/CodeResources", APP + r"/Contents/MacOS/Buffy",
    APP + r"/Contents/Frameworks/(libSDL3\.0|libMoltenVK)\.dylib",
    APP + r"/Contents/Resources/(AppIcon\.icns|settings\.json)",
    APP + r"/Contents/Resources/licenses/" + NAME,
    APP + r"/Contents/Resources/licenses/(sdl3|moltenvk|glslang|spirv-tools|spirv-headers|vulkan-headers|llvm|zydis)/" + NAME,
    APP + r"/Contents/Resources/compiler/(clang|ld64\.lld|xbe_recomp|buffy_config|sdk_names\.txt|overrides\.txt)",
    APP + r"/Contents/Resources/compiler/include/cpu/(x86_ops\.h|x86_context\.h)",
    APP + r"/Contents/Resources/compiler/include/freestanding/(cmath|cstdint|cstring|limits|type_traits)",
    APP + r"/Contents/Resources/compiler/xdk/4432/layout\.toml",
]
# files that have to be there
R = APP + "/Contents/"
REQUIRED = [
    "README.txt", "recomplauncher.json", R + "Info.plist", R + "_CodeSignature/CodeResources", R + "MacOS/Buffy",
    R + "Frameworks/libSDL3.0.dylib", R + "Frameworks/libMoltenVK.dylib", R + "Resources/AppIcon.icns", R + "Resources/settings.json",
    R + "Resources/licenses/THIRD_PARTY.txt", R + "Resources/licenses/Hellmouth-LICENSE.txt",
    R + "Resources/licenses/LiberationSans-OFL.txt", R + "Resources/licenses/SMAA-LICENSE.txt",
    R + "Resources/compiler/clang", R + "Resources/compiler/ld64.lld", R + "Resources/compiler/xbe_recomp",
    R + "Resources/compiler/buffy_config", R + "Resources/compiler/sdk_names.txt", R + "Resources/compiler/overrides.txt",
    R + "Resources/compiler/include/cpu/x86_ops.h", R + "Resources/compiler/include/cpu/x86_context.h",
    R + "Resources/compiler/include/freestanding/cmath", R + "Resources/compiler/include/freestanding/cstdint",
    R + "Resources/compiler/include/freestanding/cstring", R + "Resources/compiler/include/freestanding/limits",
    R + "Resources/compiler/include/freestanding/type_traits", R + "Resources/compiler/xdk/4432/layout.toml",
] + [R + "Resources/licenses/%s/SOURCE.txt" % c for c in ("sdl3", "moltenvk", "glslang", "spirv-tools", "spirv-headers", "vulkan-headers", "llvm", "zydis")]
# the Mach-O files: the only ones there may be, and every one of them is in the release
MACHO = [R + "MacOS/Buffy", R + "Frameworks/libSDL3.0.dylib", R + "Frameworks/libMoltenVK.dylib", R + "Resources/compiler/clang",
         R + "Resources/compiler/ld64.lld", R + "Resources/compiler/xbe_recomp", R + "Resources/compiler/buffy_config"]
EXECUTABLE = [R + "MacOS/Buffy", R + "Resources/compiler/clang", R + "Resources/compiler/ld64.lld", R + "Resources/compiler/xbe_recomp",
              R + "Resources/compiler/buffy_config"]
# names of what is made from the game, wherever they are
FORBIDDEN_NAMES = re.compile(
    r"(\.xbe|\.iso|\.xpr|\.pak|\.wma|\.wmv|\.obj|\.o|\.lnk|\.dylib\.stamp)$|^(final\.map|game\.toml|image_symbols\.h|exports\.def|imports\.def|code\.lib|"
    r"Buffy\.dll|Buffy\.dylib|x86_recomp.*|x86_func_mapping.*|disc_image\.txt|abi_inputs\.txt|game_code_flags\.h"
    r"|Buffy\.ico|Buffy\.png|Buffy\.icns|pictures\.stamp|achievements|icon\.png|\.DS_Store|\._.*)$", re.I)
FORBIDDEN_FOLDERS = {"achievements", "__macosx"}
STAMP = re.compile(rb"xbe-game-code abi=[0-9a-z]+ image=[0-9a-f]+ symbols=[0-9a-f]+")
MACHO_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def text_section(path):
    """The raw bytes of a thin arm64 Mach-O's __TEXT,__text: where its code is."""
    with open(path, "rb") as f:
        data = f.read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("%s is not a 64 bit Mach-O" % path)
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, at)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, at + 64)[0]
            for i in range(nsects):
                s = at + 72 + i * 80
                sect, seg = data[s:s + 16].rstrip(b"\0"), data[s + 16:s + 32].rstrip(b"\0")
                length, offset = struct.unpack_from("<QI", data, s + 40)
                if sect == b"__text" and seg == b"__TEXT":
                    return data[offset:offset + length]
        at += size
    raise SystemExit("no __TEXT,__text in " + path)


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


def run(*cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def machine_patterns():
    """The names of this machine that no file may hold, whole: /Users/<name>/, the home folder, the user, the e-mail's name, the host."""
    pats = [("a /Users/<name>/ path", rb"/Users/[^/\0\s]+/")]
    def add(label, value):
        if value and len(value) >= 4:
            pats.append((label, re.escape(value.encode())))
    add("the home folder", os.path.expanduser("~"))
    add("the user name", getpass.getuser())
    rc, mail = run("git", "config", "user.email")
    if rc == 0:
        add("the name part of git's user.email", mail.strip().split("@")[0])
    host = socket.gethostname()
    add("the host's name", host)
    add("the host's short name", host.split(".")[0])
    return pats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--dylib", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--generated", required=True)
    ap.add_argument("--abi-inputs", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--zip")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    a = ap.parse_args()
    folder = os.path.abspath(a.folder)
    repo = os.path.abspath(a.repo)
    problems = []

    # 1. the files
    files, links = [], []
    for root, dirs, names in os.walk(folder):
        for d in dirs:
            if os.path.islink(os.path.join(root, d)):
                links.append(os.path.relpath(os.path.join(root, d), folder))
        for n in names:
            p = os.path.join(root, n)
            if os.path.islink(p):
                links.append(os.path.relpath(p, folder))
            files.append(os.path.relpath(p, folder).replace(os.sep, "/"))
    files.sort()
    for l in links:
        if not l.startswith(APP + "/"):
            problems.append("a link outside the app: " + l)
        else:
            problems.append("a link in the app (there should be none): " + l)
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
        elif FORBIDDEN_FOLDERS & {part.lower() for part in f.split("/")[:-1]}:
            problems.append("in a folder of something made from the game: " + f)

    # 3. the game's code and data, and 5. the machine, looked for in every file
    dylib_text = text_section(a.dylib)
    with open(a.xbe, "rb") as f:
        xbe = f.read()
    probes = [("code of Buffy.dylib", w) for w in windows(dylib_text, 64, 48)]
    probes += [("Default.xbe", w) for w in windows(xbe, 64, 48, start=0x1000)]
    lines = []
    for n in sorted(os.listdir(a.generated)):
        if n.startswith("x86_recomp.") and n.endswith(".cpp") and len(lines) < 64:
            with open(os.path.join(a.generated, n), "rb") as f:
                body = [l.strip() for l in f.read().split(b"\n")]
            body = [l for l in body if len(l) >= 40 and re.search(rb"(sub_|0x)[0-9A-Fa-f]{6,8}", l)]
            if body:
                lines.append(body[len(body) // 2])
    probes += [("generated C++", l) for l in lines]
    print("looking for %d pieces of the game's code and data (%d + %d byte windows of 48 bytes, %d lines) in %d files" %
          (len(probes), 64, 64, len(lines), len(files)))
    if len(probes) < 128:
        problems.append("only %d probes of the game's code could be made: the dylib or the xbe given is too small to look for" % len(probes))
    patterns = machine_patterns()
    print("looking for the names of this machine: %s" % ", ".join(label for label, _ in patterns))
    for f in files:
        p = os.path.join(folder, f)
        if os.path.islink(p) or os.path.getsize(p) == 0:
            continue
        with open(p, "rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as m:
            for what, w in probes:
                at = m.find(w)
                if at >= 0:
                    problems.append("%s holds a piece of %s (at byte %d)" % (f, what, at))
                    break
            for label, pat in patterns:
                hit = re.search(pat, m)
                if hit:
                    problems.append("%s names the machine or user that built it (%s): %r" % (f, label, bytes(hit.group(0))[:60]))
                    break
            # a Mach-O that is not one of the app's own
            if bytes(m[:4]) in MACHO_MAGIC and f not in MACHO:
                problems.append("a Mach-O file that is not one of the app's own: " + f)

    # 4. the headers are the ones the ABI token covers; the stamp is the dylib's
    hashes = {}
    for line in open(a.abi_inputs, encoding="utf-8"):
        m = re.match(r"^([0-9a-f]{40}) (\S+)$", line.strip())
        if m:
            hashes[m.group(2)] = m.group(1)
    checked = 0
    prefix = R + "Resources/compiler/include/"
    for rel in files:
        if rel.startswith(prefix):
            key = "sdk/runtime/cpu/" + rel[len(prefix):].replace("cpu/", "", 1)
            want = hashes.get(key)
            if want is None:
                problems.append("%s is not among the ABI token's inputs (%s)" % (rel, key))
            elif sha1(os.path.join(folder, rel)) != want:
                problems.append("%s differs from the file the ABI token was computed from" % rel)
            else:
                checked += 1
    print("%d headers equal the ones in the ABI token's inputs" % checked)
    exe_path = os.path.join(folder, R + "MacOS/Buffy")
    if os.path.isfile(exe_path):
        with open(exe_path, "rb") as f:
            exe_stamps = set(STAMP.findall(f.read()))
        with open(a.dylib, "rb") as f:
            dylib_stamps = set(STAMP.findall(f.read()))
        print("Buffy stamp:       %s" % sorted(s.decode() for s in exe_stamps))
        print("Buffy.dylib stamp: %s" % sorted(s.decode() for s in dylib_stamps))
        if len(exe_stamps) != 1 or exe_stamps != dylib_stamps:
            problems.append("the executable's stamp is not the dylib's")

    # 6. Info.plist, recomplauncher.json, settings.json
    title = re.search(r"\.title_id = 0x([0-9A-Fa-f]{8})", open(os.path.join(repo, "src", "common", "game_info.cpp"), encoding="utf-8").read())
    plist = {}
    try:
        with open(os.path.join(folder, R + "Info.plist"), "rb") as f:
            plist = plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        problems.append("Info.plist cannot be read: %s" % e)
    want = {"CFBundleExecutable": "Buffy", "CFBundleIdentifier": "org.xboxsdk." + title.group(1).lower() if title else None,
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": a.version, "CFBundleVersion": a.version, "CFBundleIconFile": "AppIcon",
            "LSMinimumSystemVersion": "13.0", "NSHighResolutionCapable": True, "LSApplicationCategoryType": "public.app-category.action-games"}
    for key, value in want.items():
        if plist and plist.get(key) != value:
            problems.append("Info.plist: %s is %r, wanted %r" % (key, plist.get(key), value))
    manifest_path = os.path.join(folder, "recomplauncher.json")
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path, encoding="utf-8") as f:
                text = f.read()
            data = json.loads(text)
            if "@" in text:
                problems.append("recomplauncher.json has an @ in it: a placeholder of tools/recomplauncher-mac.json.in was left")
            if data.get("prepare", {}).get("version") != a.version:
                problems.append("recomplauncher.json: the setup's version is %r, the release's is %s" % (data.get("prepare", {}).get("version"), a.version))
            for step in ("start", "prepare"):
                program = (data.get(step, {}).get("program") or [""])[0]
                if program != APP + "/Contents/MacOS/Buffy" or not os.access(os.path.join(folder, program), os.X_OK):
                    problems.append("recomplauncher.json: %s runs %r, which is not the app's executable" % (step, program))
            if set(data.get("system", {})) != {"macos"} or data["system"]["macos"].get("version") != plist.get("LSMinimumSystemVersion", "").split(".")[0]:
                problems.append("recomplauncher.json: system is %r, not the macOS the app's Info.plist says (%s)" % (data.get("system"), plist.get("LSMinimumSystemVersion")))
        except ValueError as e:
            problems.append("recomplauncher.json is not JSON: %s" % e)
    try:
        with open(os.path.join(folder, R + "Resources/settings.json"), encoding="utf-8") as f:
            keys = {"%s/%s" % (s["section"], s["key"]) for s in json.load(f)["settings"]}
        if not keys or "graphics/renderer" in keys:
            problems.append("Resources/settings.json lists %d settings (%s)" % (len(keys), "the Windows renderer is one" if "graphics/renderer" in keys else "none"))
    except (OSError, ValueError, KeyError) as e:
        problems.append("Resources/settings.json cannot be read: %s" % e)

    # 7. the Mach-O files, and the signature
    app = os.path.join(folder, APP)
    for f in MACHO:
        if f in files and bytes(open(os.path.join(folder, f), "rb").read(4)) not in MACHO_MAGIC:
            problems.append("not a Mach-O file: " + f)
    for f in EXECUTABLE:
        if f in files and not os.access(os.path.join(folder, f), os.X_OK):
            problems.append("not executable: " + f)
    if os.path.isdir(app):
        rc, out = run("bash", os.path.join(repo, "tools", "mac", "check_mac_deps.sh"), app)
        passes = len(re.findall(r"^PASS", out, re.M))
        print("tools/mac/check_mac_deps.sh on the app: %d PASS%s" % (passes, "" if rc == 0 else ", FAILED"))
        if rc != 0 or "ALL PASSED" not in out:
            problems.append("tools/mac/check_mac_deps.sh failed on the app:\n      " + "\n      ".join(l for l in out.splitlines() if l.startswith("FAIL"))[:1500])
        rc, out = run("codesign", "--verify", "--deep", "--strict", "--verbose=2", app)
        print("codesign --verify --deep --strict: %s" % ("verified" if rc == 0 else "FAILED"))
        if rc != 0:
            problems.append("the app does not verify: " + " / ".join(out.strip().splitlines()[-2:])[:400])
        rc, out = run("codesign", "-dvv", app)
        flags = re.search(r"flags=0x[0-9a-f]+\(([^)]*)\)", out)
        if "Signature=adhoc" not in out or (flags and "runtime" in flags.group(1)):
            problems.append("the app is not signed ad hoc without the hardened runtime: " + (flags.group(0) if flags else out.strip()[-200:]))
        else:
            print("signature: ad hoc, %s" % (flags.group(0) if flags else "no flags"))

    # 8. the zip
    if a.zip:
        with zipfile.ZipFile(a.zip) as z:
            top = None
            names = []
            for info in z.infolist():
                if info.is_dir():
                    continue
                mode = info.external_attr >> 16
                if mode & 0o170000 == 0o120000:
                    problems.append("a link in the zip: " + info.filename)
                parts = info.filename.split("/", 1)
                top = top or parts[0]
                if parts[0] != top or len(parts) < 2:
                    problems.append("outside the zip's one folder: " + info.filename)
                    continue
                if parts[1] in EXECUTABLE and not mode & 0o111:
                    problems.append("not executable in the zip: " + parts[1])
                names.append(parts[1])
            if sorted(names) != files:
                problems.append("the zip's files are not the folder's: " + ", ".join(sorted(set(names) ^ set(files))[:5]))
            print("zip: %s, %d bytes, %d files in %s/" % (a.zip, os.path.getsize(a.zip), len(names), top))
            if top and a.version not in top:
                problems.append("the zip's folder %s does not carry the version %s" % (top, a.version))
            if top and "macos-arm64" not in top:
                problems.append("the zip's folder %s does not say macos-arm64 (RecompLauncher picks a Mac file by the name)" % top)
            if "macos-arm64" not in os.path.basename(a.zip):
                problems.append("the zip's name %s does not say macos-arm64" % os.path.basename(a.zip))

    if problems:
        print("\nAUDIT FAILED:")
        for p in problems:
            print("  - " + p)
        return 1
    print("\nAUDIT PASSED: only the allowed files, none made from the game, none naming the build machine")
    return 0


if __name__ == "__main__":
    sys.exit(main())
