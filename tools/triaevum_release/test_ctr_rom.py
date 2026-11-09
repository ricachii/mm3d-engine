from __future__ import annotations

import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

from ctr_rom import (
    CtrRomError,
    extract_decrypted_rom,
    is_supported_rom_path,
    publish_extracted_inputs,
)


PARTITION_BASE = 0x4000
PARTITION_SIZE = 0x4000
CODE = b"synthetic-uncompressed-exefs-code"
PROGRAM_ID = 0x0004000000033600


def _u32_into(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def _build_rom(path: Path, *, declares_decrypted: bool = True) -> bytes:
    image = bytearray(PARTITION_BASE + PARTITION_SIZE)
    image[0x100:0x104] = b"NCSD"
    _u32_into(image, 0x120, PARTITION_BASE // 0x200)
    _u32_into(image, 0x124, PARTITION_SIZE // 0x200)

    ncch = PARTITION_BASE
    image[ncch + 0x100 : ncch + 0x104] = b"NCCH"
    _u32_into(image, ncch + 0x108, PARTITION_SIZE // 0x200)
    struct.pack_into("<Q", image, ncch + 0x118, PROGRAM_ID)
    _u32_into(image, ncch + 0x180, 0x400)
    image[ncch + 0x18D] = 0x02
    image[ncch + 0x18F] = 0x04 if declares_decrypted else 0

    exheader = ncch + 0x200
    image[exheader : exheader + 0x800] = bytes(index & 0xFF for index in range(0x800))
    image[exheader + 0x0D] = 0

    exefs = ncch + 0xC00
    _u32_into(image, ncch + 0x1A0, 0xC00 // 0x200)
    _u32_into(image, ncch + 0x1A4, 2)
    image[exefs : exefs + 8] = b".code\0\0\0"
    _u32_into(image, exefs + 8, 0)
    _u32_into(image, exefs + 12, len(CODE))
    image[exefs + 0x200 : exefs + 0x200 + len(CODE)] = CODE

    romfs = ncch + 0x2000
    _u32_into(image, ncch + 0x1B0, 0x2000 // 0x200)
    _u32_into(image, ncch + 0x1B4, 0x2000 // 0x200)
    image[romfs : romfs + 4] = b"IVFC"
    image[romfs + 4 : romfs + 0x2000] = b"R" * (0x2000 - 4)
    path.write_bytes(image)
    return bytes(image[exheader : exheader + 0x800])



def _build_cia(
    path: Path,
    *,
    encrypted: bool = False,
    title_type: int = 0x00040000,
    extra_content: bool = True,
    meta_after_content: bool = False,
) -> None:
    """Wrap the synthetic NCCH in a minimal CIA container (no real keys)."""

    scratch = path.with_suffix(".scratch.3ds")
    _build_rom(scratch)
    ncch = scratch.read_bytes()[PARTITION_BASE:]
    scratch.unlink()

    def align(value: int) -> int:
        return (value + 0x3F) // 0x40 * 0x40

    certificate = b"C" * 0x100
    ticket = b"T" * 0x150
    count = 2 if extra_content else 1
    tmd = bytearray(0x140 + 0xC4 + 0x900 + 0x30 * count)
    struct.pack_into(">I", tmd, 0, 0x010004)
    struct.pack_into(">II", tmd, 0x140 + 0x4C, title_type, 0x00125500)
    struct.pack_into(">H", tmd, 0x140 + 0x9E, count)
    chunk_type = 0x0001 if encrypted else 0x0000
    base = 0x140 + 0xC4 + 0x900
    struct.pack_into(">IHHQ", tmd, base, 0, 0, chunk_type, len(ncch))
    if extra_content:
        struct.pack_into(">IHHQ", tmd, base + 0x30, 1, 1, 0, 0x200)
    content = ncch + (b"M" * 0x200 if extra_content else b"")

    header = bytearray(0x2020)
    meta = b"X" * 0x100 if meta_after_content else b""
    struct.pack_into("<IHHIIII", header, 0, 0x2020, 0, 0, len(certificate), len(ticket), len(tmd), len(meta))
    struct.pack_into("<Q", header, 0x18, len(content))
    out = bytearray(header)
    for part in (certificate, ticket, bytes(tmd)):
        out += b"\0" * (align(len(out)) - len(out))
        out += part
    out += b"\0" * (align(len(out)) - len(out))
    out += content
    path.write_bytes(bytes(out))

class CtrRomTests(unittest.TestCase):
    def test_accepts_3ds_and_cci_as_the_same_ncsd_container(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for suffix in (".3ds", ".cci", ".CCI"):
                with self.subTest(suffix=suffix):
                    rom = root / f"game{suffix}"
                    expected_exheader = _build_rom(rom)
                    output = (
                        root / f"out-{suffix[1:].lower()}-{len(list(root.iterdir()))}"
                    )
                    extracted = extract_decrypted_rom(rom, output)

                    self.assertEqual(extracted.container_kind, "NCSD")
                    self.assertEqual(extracted.partition_index, 0)
                    self.assertEqual(extracted.program_id, PROGRAM_ID)
                    self.assertEqual(extracted.code.path.read_bytes(), CODE)
                    self.assertEqual(
                        extracted.exheader.path.read_bytes(), expected_exheader
                    )
                    self.assertEqual(extracted.romfs.bytes, 0x2000)
                    self.assertEqual(
                        extracted.romfs.sha256,
                        hashlib.sha256(b"IVFC" + b"R" * (0x2000 - 4)).hexdigest(),
                    )

    def test_accepts_plaintext_when_legacy_crypto_flag_is_retained(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rom = root / "legacy-flag.cci"
            _build_rom(rom, declares_decrypted=False)
            extracted = extract_decrypted_rom(rom, root / "output")
            self.assertEqual(extracted.code.path.read_bytes(), CODE)

    def test_accepts_a_decrypted_cia_and_extracts_the_same_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _build_cia(root / "game.cia")
            extracted = extract_decrypted_rom(root / "game.cia", root / "out")
            self.assertEqual(extracted.container_kind, "CIA")
            self.assertEqual(extracted.program_id, PROGRAM_ID)
            self.assertEqual(extracted.code.path.read_bytes(), CODE)

    def test_accepts_a_decrypted_cia_with_the_meta_section_after_the_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _build_cia(root / "game.cia", meta_after_content=True)
            extracted = extract_decrypted_rom(root / "game.cia", root / "out")
            self.assertEqual(extracted.code.path.read_bytes(), CODE)

    def test_rejects_a_cia_whose_content_is_encrypted_with_the_title_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _build_cia(root / "game.cia", encrypted=True)
            with self.assertRaisesRegex(CtrRomError, "title key"):
                extract_decrypted_rom(root / "game.cia", root / "out")
            self.assertFalse((root / "out").exists())

    def test_accepts_a_decrypted_update_cia(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _build_cia(root / "update.cia", title_type=0x0004000E, extra_content=False)
            extracted = extract_decrypted_rom(root / "update.cia", root / "out")
            self.assertEqual(extracted.container_kind, "CIA")

    def test_rejects_downloadable_content_cia(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _build_cia(root / "dlc.cia", title_type=0x0004008C)
            with self.assertRaisesRegex(CtrRomError, "downloadable content"):
                extract_decrypted_rom(root / "dlc.cia", root / "out")

    def test_cia_and_cxi_suffixes_are_supported(self) -> None:
        self.assertTrue(is_supported_rom_path(Path("game.cia")))
        self.assertTrue(is_supported_rom_path(Path("game.CXI")))

    def test_public_import_requires_a_rom_extension(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rom = root / "game.bin"
            _build_rom(rom)
            with self.assertRaisesRegex(CtrRomError, r"\.3ds, \.cci, \.cia or \.cxi"):
                extract_decrypted_rom(rom, root / "output")
            self.assertFalse((root / "output").exists())

    def test_supported_suffix_check_is_case_insensitive(self) -> None:
        self.assertTrue(is_supported_rom_path(Path("game.3DS")))
        self.assertTrue(is_supported_rom_path(Path("game.CCI")))
        self.assertFalse(is_supported_rom_path(Path("game.iso")))

    def test_publishes_inputs_under_a_stable_content_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first_rom = root / "first.cci"
            _build_rom(first_rom)
            first_staging = root / "first-staging"
            first = publish_extracted_inputs(
                extract_decrypted_rom(first_rom, first_staging), root / "sources"
            )
            self.assertFalse(first_staging.exists())

            second_rom = root / "second.3ds"
            _build_rom(second_rom)
            second_staging = root / "second-staging"
            second = publish_extracted_inputs(
                extract_decrypted_rom(second_rom, second_staging), root / "sources"
            )
            self.assertFalse(second_staging.exists())
            self.assertEqual(first.code.path, second.code.path)
            self.assertEqual(first.romfs.path, second.romfs.path)


if __name__ == "__main__":
    unittest.main()
