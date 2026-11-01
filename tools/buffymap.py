"""Reads final.map, the linker map of this exact Default.xbe.

The map's Rva+Base column is a PE link address and does not match the XBE, so an address is the XBE section's VA plus
the map's section offset. The map's section numbers line up with the XBE's sections only through this table.
"""
import re
from dataclasses import dataclass

# map section number -> (XBE section, XBE VA)
SECTIONS = {
    0x1: (".text", 0x00011000),
    0x2: ("D3D", 0x005913A0),
    0x3: ("D3DX", 0x005A29C0),
    0x4: ("XGRPH", 0x005A4BC0),
    0x5: ("DSOUND", 0x005A6BE0),
    0x6: ("WMADECXM", 0x005C38C0),
    0x7: ("WMADEC", 0x005C4100),
    0x8: ("WMVDEC", 0x005DCE80),
    0x9: ("XPP", 0x0060F740),
    0xA: (".rdata", 0x00616D80),
    0xB: (".data", 0x00750EA0),
    0xC: ("DOLBY", 0x009163C0),
}
CODE_SECTIONS = [".text", "D3D", "D3DX", "XGRPH", "DSOUND", "WMADECXM", "WMADEC", "WMVDEC", "XPP"]

_SEG = re.compile(r"^ ([0-9a-f]{4}):([0-9a-f]{8}) ([0-9a-f]{8})H (\S+)\s+(CODE|DATA)\s*$")
_SYM = re.compile(r"^ ([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+[0-9a-f]{8}\s+(f\s+)?(i\s+)?(\S+)\s*$")


@dataclass
class Symbol:
    name: str
    va: int
    section: int
    is_func: bool
    lib: str  # "" for the game's own objects
    obj: str


class Map:
    def __init__(self, path):
        self.segments = []  # (section number, offset, length, name)
        self.symbols = []
        with open(path, errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                m = _SEG.match(line)
                if m:
                    self.segments.append((int(m[1], 16), int(m[2], 16), int(m[3], 16), m[4]))
                    continue
                m = _SYM.match(line)
                if not m:
                    continue
                sec = int(m[1], 16)
                if sec not in SECTIONS:
                    continue
                lib, _, obj = m[6].rpartition(":")
                self.symbols.append(Symbol(m[3], SECTIONS[sec][1] + int(m[2], 16), sec, bool(m[4]), lib, obj))
        self.by_name = {}
        self.by_va = {}
        for s in self.symbols:
            self.by_name.setdefault(s.name, s)
            self.by_va.setdefault(s.va, s)
        self._sorted = sorted(self.by_va)

    def seg_range(self, section, name):
        for sec, off, length, seg in self.segments:
            if sec == section and seg == name:
                base = SECTIONS[sec][1]
                return base + off, base + off + length
        raise KeyError(name)

    def nearest(self, va):
        """The symbol at or below va, as (symbol, distance)."""
        import bisect
        i = bisect.bisect_right(self._sorted, va) - 1
        if i < 0:
            return None, 0
        s = self.by_va[self._sorted[i]]
        return s, va - s.va


def short_name(name):
    """A readable form of a decorated name: ?KickOff@CDevice@D3D@@QAEXXZ -> D3D::CDevice::KickOff."""
    if name.startswith("??_7"):
        return "vtable " + "::".join(reversed(re.match(r"\?\?_7(.*?)@@", name)[1].split("@")))
    if name.startswith("?"):
        m = re.match(r"\?([^@]+)@(.*?)@@", name)
        if m:
            return "::".join(list(reversed([p for p in m[2].split("@") if p])) + [m[1]])
    return re.sub(r"@\d+$", "", name).lstrip("_@")
