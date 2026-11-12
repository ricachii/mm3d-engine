#!/usr/bin/env python3
"""Merge a decrypted base-game CIA and a decrypted update CIA into one decrypted .cci.

Majora's Mask 3D USA: the base CIA (v1.0.0, title 0004000000125500) plus the update
CIA (v1.1.0, title 0004000E00125500) become what a v1.1 cartridge dump contains: the
update's ExHeader + ExeFS (the new code) and the base RomFS plus the files the update
adds or replaces. The result is an NCSD (.cci) that mm3d-pc's setup reads.

Only open-source building blocks are used (romfs_replace.py of this repository); no ROM,
key or asset is downloaded. Both inputs must already be DECRYPTED (for example with
GodMode9's "Decrypt file" on the CIA): the tool refuses encrypted content. The NCCH/NCSD
signatures are not recomputed (they cannot be, and decrypted dumps are not verified by
emulators or by this port).

usage: make_cci.py <base.cia> <update.cia> <out.cci>
"""
from __future__ import annotations

import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

UNIT = 0x200
HERE = Path(__file__).resolve().parent


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) // boundary * boundary


def find_ncch(path: Path) -> int:
    """Offset of the NCCH (content 0) of a CIA: after the header, certificate chain,
    ticket and TMD (64-byte aligned), with the meta section before or after the content."""
    with open(path, "rb") as source:
        header = source.read(0x2040)
    header_size, _type, _version, certs, ticket, tmd, meta, _content = struct.unpack_from("<IHHIIIIQ", header, 0)
    start = align(header_size, 64) + align(certs, 64) + align(ticket, 64) + align(tmd, 64)
    with open(path, "rb") as source:
        for offset in (start, start + align(meta, 64)):
            source.seek(offset + 0x100)
            if source.read(4) == b"NCCH":
                return offset
    sys.exit(f"{path.name}: no NCCH found at the expected CIA content offsets")


class Ncch:
    def __init__(self, path: Path):
        self.path = path
        self.offset = find_ncch(path)
        with open(path, "rb") as source:
            source.seek(self.offset)
            self.header = bytearray(source.read(0x200))
            self.exheader = source.read(0x800)
        if not self.header[0x188 + 7] & 4:
            sys.exit(f"{path.name} is encrypted: decrypt it first (GodMode9: CIA image options > Decrypt file)")

    def region(self, field: int) -> tuple[int, int]:
        offset, size = struct.unpack_from("<II", self.header, field)
        return offset * UNIT, size * UNIT

    def read(self, field: int) -> bytes:
        offset, size = self.region(field)
        with open(self.path, "rb") as source:
            source.seek(self.offset + offset)
            return source.read(size)


def main() -> int:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base, update = Ncch(Path(sys.argv[1])), Ncch(Path(sys.argv[2]))
    out = Path(sys.argv[3])
    if base.header[0x118:0x120] != bytes.fromhex("0055120000000400"):
        sys.exit("the first CIA is not Majora's Mask 3D (0004000000125500)")
    if update.header[0x118:0x120] != bytes.fromhex("005512000e000400"):
        sys.exit("the second CIA is not its update (0004000E00125500)")

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        # RomFS: base + the files the update adds or replaces
        base_romfs = work / "base_romfs.bin"
        offset, size = base.region(0x1B0)
        with open(base.path, "rb") as source, open(base_romfs, "wb") as target:
            source.seek(base.offset + offset)
            left = size
            while left:
                chunk = source.read(min(left, 1 << 24))
                target.write(chunk)
                left -= len(chunk)
        replacements = []
        update_romfs = update.read(0x1B0)
        for name, file_offset, file_size in list_romfs(update_romfs):
            source_file = work / ("u_" + hashlib.sha1(name.encode()).hexdigest())
            source_file.write_bytes(update_romfs[0x1000 + data_offset(update_romfs) + file_offset:][:file_size])
            replacements.append(f"{name}={source_file}")
        merged = work / "romfs.bin"
        subprocess.run([sys.executable, str(HERE / "romfs_replace.py"), str(base_romfs), str(merged), *replacements],
                       check=True)
        romfs = merged.read_bytes()

        # NCCH: header of the base, ExHeader and ExeFS of the update, merged RomFS
        exheader = bytearray(update.exheader)
        exheader[0x1C8:0x1D0] = base.exheader[0x1C8:0x1D0]            # jump id of the base title
        exefs = update.read(0x1A0)
        logo, plain = base.read(0x198), base.read(0x190)
        header = bytearray(base.header)
        cursor = 1 + len(exheader) // UNIT                              # after header + ExHeader
        layout = {}
        for field, blob in ((0x198, logo), (0x190, plain), (0x1A0, exefs)):
            layout[field] = (cursor, len(blob) // UNIT)
            cursor += len(blob) // UNIT
        cursor = align(cursor, 8)
        layout[0x1B0] = (cursor, len(romfs) // UNIT)
        cursor += len(romfs) // UNIT
        for field, (start, units) in layout.items():
            struct.pack_into("<II", header, field, start, units)
        struct.pack_into("<I", header, 0x104, cursor)                   # NCCH size in media units
        header[0x160:0x180] = hashlib.sha256(exheader[:0x400]).digest()
        header[0x1C0:0x1E0] = hashlib.sha256(exefs[:UNIT]).digest()
        header[0x1E0:0x200] = hashlib.sha256(romfs[:UNIT]).digest()

        ncch_units = cursor
        first = 0x20                                                     # NCCH at 0x4000, as in retail dumps
        # Partition 7 keeps the update's own NCCH (its RomFS is the "update RomFS" the game opens with
        # fs:USER OpenFileDirectly file path type 5). It starts on a 0x80-unit boundary after partition 0.
        update_start = align(first + ncch_units, 0x80)
        with open(update.path, "rb") as source:
            source.seek(update.offset)
            update_ncch = source.read(struct.unpack_from("<I", update.header, 0x104)[0] * UNIT)
        ncsd = bytearray(first * UNIT)
        ncsd[0x100:0x104] = b"NCSD"
        struct.pack_into("<I", ncsd, 0x104, update_start + len(update_ncch) // UNIT)
        ncsd[0x108:0x110] = header[0x108:0x110]                          # media id = title id
        ncsd[0x110] = 1                                                  # partition 0: executable content
        ncsd[0x118] = 3                                                  # (no encryption of the container)
        struct.pack_into("<II", ncsd, 0x120, first, ncch_units)
        struct.pack_into("<II", ncsd, 0x120 + 7 * 8, update_start, len(update_ncch) // UNIT)
        ncsd[0x110 + 7] = 1
        ncsd[0x188 + 5] = 2                                              # media type: card1
        ncsd[0x188 + 6] = 0                                              # media unit 0x200
        with open(out, "wb") as target:
            target.write(ncsd)
            target.write(header)
            target.write(exheader)
            for blob in (logo, plain, exefs):
                target.write(blob)
            target.write(b"\0" * ((layout[0x1B0][0] - layout[0x1A0][0] - layout[0x1A0][1]) * UNIT))
            target.write(romfs)
            target.write(bytes((update_start - first - ncch_units) * UNIT))
            target.write(update_ncch)
    print(f"wrote {out} ({out.stat().st_size} bytes)")
    return 0


def data_offset(romfs: bytes) -> int:
    return struct.unpack_from("<10I", romfs, 0x1000)[9]


def list_romfs(romfs: bytes):
    """(path, data offset, size) of every file of a raw IVFC RomFS image (level 3 at 0x1000)."""
    base = 0x1000
    header = struct.unpack_from("<10I", romfs, base)
    dir_meta, file_meta, file_meta_size = header[3], header[7], header[8]
    files = romfs[base + file_meta: base + file_meta + file_meta_size]
    dirs = romfs[base + dir_meta: base + dir_meta + header[4]]
    result = []

    def walk(directory: int, prefix: str) -> None:
        _parent, _sibling, child, first_file, _next, name_length = struct.unpack_from("<6I", dirs, directory)
        name = dirs[directory + 24: directory + 24 + name_length].decode("utf-16le")
        path = prefix + ("/" + name if name else "")
        entry = first_file
        while entry != 0xFFFFFFFF:
            _p, sibling, file_offset, file_size, _hash, length = struct.unpack_from("<IIQQII", files, entry)
            result.append((path + "/" + files[entry + 32: entry + 32 + length].decode("utf-16le"),
                           file_offset, file_size))
            entry = sibling
        while child != 0xFFFFFFFF:
            walk(child, path)
            child = struct.unpack_from("<I", dirs, child + 4)[0]

    walk(0, "")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
