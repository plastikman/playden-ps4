#!/usr/bin/env python3
"""Make a freshly booted Wine prefix's registry byte-reproducible.

wineboot --init stamps every key with the wall clock and invents a few per-boot identifiers, so two
builds of the same runtime produce different system.reg/user.reg. That matters here because the kit
view's files_sha256 covers these files and OwnerMarker() writes that hash into every prepared game's
.playden-title-owner / .playden-stage-owner. A changed hash means TitleCurrent() is false for every
installed game, so Playden offers "Update games to the new runtime kit" after every package install
even when the runtime is identical -- and until it is taken, Play is gated behind it.

Only bookkeeping is rewritten. Key modification times and the per-boot GUIDs mean nothing to a game:
Wine itself rewrites them in each game's own copy of the prefix as soon as it runs.

HardwareInformation.MemorySize/qwMemorySize are pinned too, and that one is a judgement call worth
knowing about. wineboot fills them from the *build machine*, and it samples live memory: two builds
minutes apart on the same host produced 8.77 GB and 8.98 GB. So a guest game was being told how much
RAM the build server happened to have at link time, and the kit could never be reproducible while
that was true. PS4_GUEST_MEMORY_BYTES below is what a guest now sees; change it if a game should be
told something else.
"""
import os, re, sys
from pathlib import Path

# What a guest game is told the machine has. 8 GiB (the PS4's physical memory) by default; Wine
# writes the low 32 bits into the DWORD, as it does for any machine with more than 4 GB, so the
# default is deliberately just under 8 GiB to keep that DWORD non-zero.
#
# Overridable with PS4_GUEST_MEMORY_BYTES (decimal or 0x hex) because it is worth A/B testing:
# Mono sizes its startup reservation from reported system memory, and a reservation too large for
# the SDK debug runtime to shadow fails the title with
# SYSTEM_DEBUG_RUNTIME_SHADOW_MEMORY_ALLOC_ERROR (Hollow Knight, 2026-11-12).
PS4_GUEST_MEMORY_BYTES = int(os.environ.get("PS4_GUEST_MEMORY_BYTES", "0x1FFF00000"), 0)

EPOCH = "0"
TIME = "#time=0"
FIXED = {
    "MachineGuid": "00000000-0000-0000-0000-000000000000",
    "MachineId": "00000000-0000-0000-0000-000000000000",
    "ContainerId": "{00000000-0000-0000-0000-000000000000}",
    "VideoID": "{00000000-0000-0000-0000-000000000000}",
}
# [Some\\Key] 1794452387  ->  [Some\\Key] 0
SECTION = re.compile(r"^(\[.*\]) \d+$")
TIMELINE = re.compile(r"^#time=[0-9a-fA-F]+$")
VALUE = re.compile(r'^"([A-Za-z]+)"="[^"]*"$')
MEM_DWORD = re.compile(r'^"HardwareInformation\.MemorySize"=dword:[0-9a-fA-F]+$')
MEM_QWORD = re.compile(r'^"HardwareInformation\.qwMemorySize"=hex\(b\):(?:[0-9a-fA-F]{2},){7}[0-9a-fA-F]{2}$')
MEM_DWORD_LINE = '"HardwareInformation.MemorySize"=dword:%08x' % (PS4_GUEST_MEMORY_BYTES & 0xFFFFFFFF)
MEM_QWORD_LINE = '"HardwareInformation.qwMemorySize"=hex(b):' + ",".join(
    "%02x" % b for b in PS4_GUEST_MEMORY_BYTES.to_bytes(8, "little"))
# A FILETIME stored as a binary value, e.g. @=hex(ffff0010):e7,6b,d6,e3,51,72,dd,01 -- the same
# wall clock as #time=, in 8 little-endian bytes.
FILETIME = re.compile(r"^(.*=)hex\(ffff0010\):(?:[0-9a-fA-F]{2},){7}[0-9a-fA-F]{2}$")
ZERO_FILETIME = "hex(ffff0010):00,00,00,00,00,00,00,00"

def canonicalize(path: Path) -> int:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    out, changed = [], 0
    for line in text.split("\n"):
        m = SECTION.match(line)
        if m:
            new = f"{m.group(1)} {EPOCH}"
        elif TIMELINE.match(line):
            new = TIME
        elif FILETIME.match(line):
            new = FILETIME.match(line).group(1) + ZERO_FILETIME
        elif MEM_DWORD.match(line):
            new = MEM_DWORD_LINE
        elif MEM_QWORD.match(line):
            new = MEM_QWORD_LINE
        else:
            v = VALUE.match(line)
            new = f'"{v.group(1)}"="{FIXED[v.group(1)]}"' if v and v.group(1) in FIXED else line
        changed += new != line
        out.append(new)
    path.write_text("\n".join(out), encoding="utf-8", errors="surrogateescape")
    return changed

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        raise SystemExit(2)
    prefix = Path(sys.argv[1])
    total = 0
    for name in ("system.reg", "user.reg", "userdef.reg"):
        f = prefix / name
        if f.is_file():
            n = canonicalize(f)
            total += n
            print(f"canonicalized {name}: {n} lines")
    if not total:
        raise SystemExit(f"no registry files canonicalized under {prefix}")
