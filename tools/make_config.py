#!/usr/bin/env python3
"""Writes config/game.toml from final.map: what xbe_symbols does for a title with signatures, with real names.

Only an exact API name or an entry of ALIASES binds. A loose match could replace an internal layer the library still
needs (CDirectSoundVoice::SetPitch is not IDirectSoundBuffer_SetPitch), so a name the map lacks stays 0: the host's
override then has nothing to catch, and the code that needed it shows up in the coverage report (tools/coverage.py).
The evidence for the values is in docs/xdk-4432.md.

    make_config.py [--map assets/final.map] [--out config/game.toml] [--sdk sdk]
"""
import argparse
import glob
import re

from buffymap import CODE_SECTIONS, SECTIONS, Map, Symbol

# SDK name -> decorated name in this map where 4432 calls it something else; ?Name@Class@@ matches up to its signature
ALIASES = {
    "CDevice_KickOff": "?KickOff@CDevice@D3D@@",
    "D3DDevice_MakeSpace": "?MakeSpace@D3D@@",
    "D3D_SetFence": "?SetFence@D3D@@",
    "D3D_BlockOnTime": "?BlockOnTime@D3D@@",
    "D3D_BlockOnResource": "?BlockOnResource@D3D@@",
    "D3D_MakeRequestedSpace": "?MakeRequestedSpace@D3D@@",
    "D3D_KickOffAndWaitForIdle": "?KickOffAndWaitForIdle@D3D@@",
    "CDirectSoundStream_AddRef": "?AddRef@CDirectSoundStream@DirectSound@@UAG",
    "CDirectSoundStream_Release": "?Release@CDirectSoundStream@DirectSound@@UAG",
    "CDirectSoundStream_GetInfo": "?GetInfo@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_GetStatus": "?GetStatus@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_Process": "?Process@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_Discontinuity": "?Discontinuity@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_Flush": "?Flush@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_Pause": "?Pause@CDirectSoundStream@DirectSound@@",
    "CDirectSoundStream_SetVolume": "?SetVolume@CDirectSoundStream@DirectSound@@",
}
DATA_ALIASES = {
    "D3D_Device": "?g_Device@D3D@@",
    "D3D_pDevice": "?g_pDevice@D3D@@",
    "D3D_DirtyFlags": "_D3D__DirtyFlags",
    "D3D_RenderState": "_D3D__RenderState",
    "D3D_DeferredTextureState": "_D3D__TextureState",
    "D3D_IndexData": "_D3D__IndexData",
    "D3D_Streams": "?g_Stream@D3D@@",
    "D3D_FvfShader": "?g_FvfVertexShader@D3D@@",
    "DSound_StreamVtable": "??_7CDirectSoundStream@DirectSound@@6BIDirectSoundStream@@@",
    "DSound_StreamVtable2": "??_7CDirectSoundStream@DirectSound@@6BCDirectSoundVoice@1@@",
    "XPP_GamepadType": "_XDEVICE_TYPE_GAMEPAD_TABLE",
    "XPP_MemoryUnitType": "_XDEVICE_TYPE_MEMORY_UNIT_TABLE",
    "D3D_SimpleRenderStateEncode": "_D3DSIMPLERENDERSTATEENCODE",
}
# The CRT's math shares one body between its stack entry (_acos) and its register entry (__CIacos), and the host's crt_*
# handlers replace that body: x in st0, or for pow x and y above the return address and y in st0. The map doesn't name
# the bodies, so each is read from the register entry's call: SDK name -> (register entry, offset of the call).
CRT_BODIES = {
    "crt_acos": ("__CIacos", 0x0B),
    "crt_asin": ("__CIasin", 0x0B),
    "crt_sqrt": ("__CIsqrt", 0x0B),
    "crt_pow": ("__CIpow_default", 0x10),
}
# proven by hand from the disassembly, not named in the map: see docs/xdk-4432.md
MANUAL_DATA = {
    "D3D_DefaultVertexShader": 0x0059FD78,  # D3D::CDevice::Init 0x598968 stores g_FvfVertexShader in the device
    "D3D_DeviceVertexShaderField": 0x380,  # at this offset, CDevice_VertexShader
}

# the sections' read-only and writable parts: linked into the same XBE section as the code, and never code
DATA_SEGMENTS = [
    (2, "D3D_RD"), (2, "D3D_URW"), (3, "D3DX_RD"), (4, "XGRPH_RD"),
    (5, "DSOUND_RD"), (5, "DSOUND_RW"), (5, "DSOUND_URW"),
    (6, "WMADECXMO_RD"), (7, "WMADEC_RD"), (7, "WMADEC_RW"), (8, "WMVDECRD"), (8, "WMVDECRW"),
    (9, ".XPP$Data"), (9, ".XPPRDATA"),
]


def sdk_names(sdk):
    """Every guest name the runtime can bind, from the sources and each known XDK's signature table."""
    names = set()
    for path in glob.glob(f"{sdk}/runtime/**/*", recursive=True):
        if not re.search(r"\.(cpp|h)$", path):
            continue
        text = open(path, errors="replace").read()
        names |= set(re.findall(r"\bHLE_(?:STDCALL|FASTCALL|CDECL)\(\s*(\w+)", text))
        names |= set(re.findall(r"\bGUEST\((\w+)\)", text))
        names |= set(re.findall(r"\bSYM\((\w+)\)", text))
        names |= set(re.findall(r"\bCRT_MATH\((\w+)", text))
    for path in glob.glob(f"{sdk}/xdk/*/signatures.toml"):
        block = re.search(r"^\[symbols\]\n(.*?)(?=^\[)", open(path).read(), re.M | re.S)
        names |= set(re.findall(r"^(\w+)\s*=", block[1], re.M))
    names |= {f"D3DDevice_SetRenderState_{s}" for s in render_state_names(sdk)}
    return names - {"name"}


def render_state_names(sdk):
    text = open(f"{sdk}/runtime/xdk/d3d8.cpp", errors="replace").read()
    return set(re.findall(r"\bD3DDevice_SetRenderState_(\w+)", text))


def decorated(m, name):
    """The one symbol with this decorated name, or with this name up to its signature."""
    if name in m.by_name:
        return m.by_name[name]
    found = {s.va: s for s in m.symbols if s.name.startswith(name)}
    if len(found) != 1:
        raise SystemExit(f"{name}: {len(found)} matches in the map")
    return next(iter(found.values()))


def crt_body(m, xbe, name):
    """The address a CRT register entry calls: the host's crt_* handlers replace this body, not the named entry."""
    entry, call = CRT_BODIES[name]
    site = m.by_name[entry].va + call
    offset = 0x1000 + site - SECTIONS[1][1]
    if xbe[offset] != 0xE8:
        raise SystemExit(f"{name}: no call at {site:08X}")
    target = site + 5 + int.from_bytes(xbe[offset + 1 : offset + 5], "little", signed=True)
    return Symbol(name, target, 1, True, "LIBCMT", "")


def lookup(m, name, aliases):
    """The symbol for an SDK name: its alias, else the exact undecorated API name in a library."""
    if name in aliases:
        return decorated(m, aliases[name])
    if name.startswith("crt_"):  # a stack entry takes its arguments elsewhere: it must never bind to a crt_* handler
        return None
    for form in (name, f"_{name}"):
        if form in m.by_name and m.by_name[form].lib:
            return m.by_name[form]
    for s in m.symbols:
        if s.lib and re.fullmatch(rf"[_@]{re.escape(name)}@\d+", s.name):
            return s
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="assets/final.map")
    ap.add_argument("--xbe", default="assets/Default.xbe")
    ap.add_argument("--out", default="config/game.toml")
    ap.add_argument("--sdk", default="sdk")
    args = ap.parse_args()
    m = Map(args.map)

    symbols, absent = {}, []
    all_names = sdk_names(args.sdk)
    data_names = set(DATA_ALIASES) | set(MANUAL_DATA)
    xbe = open(args.xbe, "rb").read()
    for name in sorted(all_names - data_names):
        s = crt_body(m, xbe, name) if name in CRT_BODIES else lookup(m, name, ALIASES)
        if s:
            symbols[name] = s
        else:
            absent.append(name)

    data = {name: decorated(m, d).va for name, d in DATA_ALIASES.items()}
    data.update(MANUAL_DATA)

    ranges = []
    for sec, seg in DATA_SEGMENTS:
        lo, hi = m.seg_range(sec, seg)
        if hi > lo:
            ranges.append((lo, hi))
    ranges.sort()
    for s in m.symbols:
        if s.is_func and any(lo <= s.va < hi for lo, hi in ranges):
            raise SystemExit(f"function {s.name} at {s.va:08X} is inside a data range")

    funcs = sorted({s.va for s in m.symbols if s.is_func and SECTIONS[s.section][0] in CODE_SECTIONS} - {s.va for s in symbols.values()})
    setjmp = m.by_name["__setjmp3"].va

    with open(args.out, "w") as w:
        w.write("# Recompiler input for Default.xbe, written by tools/make_config.py from final.map\n\n[xbe]\n")
        w.write('path = "assets/Default.xbe"\ntitle = 0x45410012\nentry = 0x00579EA2\nxdk = 4432\n')
        w.write('out = "generated/game"\npartitions = 128\n')
        w.write(f"setjmp = [0x{setjmp:08X}]\n")
        w.write("code_sections = [" + ", ".join(f'"{c}"' for c in CODE_SECTIONS) + "]\n")
        w.write("data_ranges = [\n")
        for lo, hi in ranges:
            w.write(f"    [0x{lo:08X}, 0x{hi:08X}],\n")
        w.write("]\n\n# every function the map names\n[functions]\n")
        for va in funcs:
            w.write(f'0x{va:08X} = ""\n')
        w.write("\n# functions the host replaces or calls by name, 0 where the image has none\n[symbols]\n")
        for name, s in sorted(symbols.items()):
            w.write(f"{name} = 0x{s.va:08X}\n")
        for name in absent:
            w.write(f"{name} = 0\n")
        w.write("\n[data]\n")
        for name, va in sorted(data.items()):
            w.write(f"{name} = 0x{va:08X}\n")

    print(f"{len(funcs)} functions, {len(symbols)} symbols bound, {len(absent)} absent, {len(data)} data, {len(ranges)} data ranges")


if __name__ == "__main__":
    main()
