#!/usr/bin/env python3
"""Ensure create-fself's writable segment covers the linked ELF's full .bss."""
import struct
import sys


def read_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: expected little-endian ELF64")
    return data


def bss_range(data):
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
                for i in range(shnum)]
    names_section = sections[shstrndx]
    names = data[names_section[4]:names_section[4] + names_section[5]]
    for section in sections:
        name_offset, section_type, _flags, address, _offset, size = section[:6]
        end = names.find(b"\0", name_offset)
        name = names[name_offset:end].decode("ascii")
        if name == ".bss" and section_type == 8:
            return address, address + size
    raise ValueError("linked ELF has no NOBITS .bss section")


def writable_loads(data):
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        kind, flags, _offset, address, _physical, _filesz, memsz, _align = \
            struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsize)
        if kind == 1 and flags & 2:
            yield address, address + memsz


def main():
    linked = read_elf(sys.argv[1])
    converted = read_elf(sys.argv[2])
    start, end = bss_range(linked)
    if not any((load_start & ~0x3FFF) <= start and
               ((load_end + 0x3FFF) & ~0x3FFF) >= end
               for load_start, load_end in writable_loads(converted)):
        print(f".bss 0x{start:x}-0x{end:x} is not covered by a converted writable segment",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
