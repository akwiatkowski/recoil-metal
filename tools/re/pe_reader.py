#!/usr/bin/env python3
"""Minimal read-only PE32 accessor: sections, VA->bytes, and C-string reads.

Written rather than taking a `pefile` dependency because the need is small and fixed —
map a virtual address to file bytes — and because this has to keep working in a session
where installing anything is a decision rather than a reflex. It reads; it never writes.
"""

import struct
from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int   # absolute VA, image base already added
    virtual_size: int
    raw_offset: int
    raw_size: int


class PE32:
    def __init__(self, path: str):
        with open(path, "rb") as handle:
            self.data = handle.read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE image")
        section_count = struct.unpack_from("<H", self.data, pe + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]

        self.sections: list[Section] = []
        offset = pe + 24 + optional_size
        for _ in range(section_count):
            name = self.data[offset:offset + 8].rstrip(b"\0").decode("latin-1")
            vsize, vaddr, rsize, roffset = struct.unpack_from("<IIII", self.data, offset + 8)
            self.sections.append(
                Section(name, self.image_base + vaddr, vsize, roffset, rsize))
            offset += 40

    def section_of(self, va: int) -> Optional[Section]:
        for section in self.sections:
            if section.virtual_address <= va < section.virtual_address + section.virtual_size:
                return section
        return None

    def offset_of(self, va: int) -> Optional[int]:
        section = self.section_of(va)
        if section is None:
            return None
        delta = va - section.virtual_address
        # Virtual size can exceed raw size (BSS-style tail); those bytes are not in the file.
        if delta >= section.raw_size:
            return None
        return section.raw_offset + delta

    def read(self, va: int, count: int) -> Optional[bytes]:
        offset = self.offset_of(va)
        if offset is None:
            return None
        return self.data[offset:offset + count]

    def u32(self, va: int) -> Optional[int]:
        raw = self.read(va, 4)
        if raw is None or len(raw) < 4:
            return None
        return struct.unpack("<I", raw)[0]

    def cstring(self, va: int, limit: int = 128) -> Optional[str]:
        raw = self.read(va, limit)
        if not raw:
            return None
        end = raw.find(b"\0")
        if end < 0:
            return None
        try:
            return raw[:end].decode("latin-1")
        except UnicodeDecodeError:
            return None

    def identifier(self, va: int, limit: int = 128) -> Optional[str]:
        """A C string that is printable and non-empty, or None."""
        text = self.cstring(va, limit)
        if not text:
            return None
        if any(ch < " " or ch > "~" for ch in text):
            return None
        return text

    def is_code(self, va: int) -> bool:
        section = self.section_of(va)
        return section is not None and section.name == ".text"
