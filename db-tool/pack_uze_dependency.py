#!/usr/bin/env python3
"""Append and verify UzeSID's database dependency without relying on packrom parsing."""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys
from pathlib import Path

HEADER_SIZE = 512
MAX_PROGRAM_SIZE = 61440
DEPENDENCY_OFFSET = HEADER_SIZE + MAX_PROGRAM_SIZE
PROGRAM_SIZE_OFFSET = 0x08
DEPENDENCY_LENGTH_OFFSET = 0x195
COPY_CHUNK = 1024 * 1024


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"ERROR: {message}")


def copy_bytes(src, dst, count: int) -> None:
    remaining = count
    while remaining:
        block = src.read(min(COPY_CHUNK, remaining))
        if not block:
            fail("unexpected end of input UZE")
        dst.write(block)
        remaining -= len(block)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pad a packed UZE to 60 KiB, append the LIDX/USDC database, and verify it"
    )
    parser.add_argument("uze", type=Path)
    parser.add_argument("dependency", type=Path)
    args = parser.parse_args()

    uze = args.uze.resolve()
    dependency = args.dependency.resolve()
    if not uze.is_file():
        fail(f"packed UZE does not exist: {uze}")
    if not dependency.is_file():
        fail(f"database dependency does not exist: {dependency}")

    dep_size = dependency.stat().st_size
    if dep_size < 64:
        fail("database dependency is too small")
    if dep_size > 0xFFFFFFFF:
        fail("database dependency exceeds the UZE 32-bit length field")
    with dependency.open("rb") as dep:
        if dep.read(4) != b"LIDX":
            fail("database dependency does not begin with LIDX")

    with uze.open("rb") as src:
        header = bytearray(src.read(HEADER_SIZE))
        if len(header) != HEADER_SIZE or header[:6] != b"UZEBOX":
            fail("input is not a valid UZE file")
        program_size = struct.unpack_from("<I", header, PROGRAM_SIZE_OFFSET)[0]
        if not 1 <= program_size <= MAX_PROGRAM_SIZE:
            fail(f"invalid program size in UZE header: {program_size}")
        base_size = HEADER_SIZE + program_size
        if uze.stat().st_size < base_size:
            fail("UZE is shorter than its header program-size field")

        struct.pack_into("<I", header, DEPENDENCY_LENGTH_OFFSET, dep_size)
        tmp = uze.with_name(uze.name + ".packing")
        try:
            with tmp.open("wb") as dst:
                dst.write(header)
                copy_bytes(src, dst, program_size)
                pad = DEPENDENCY_OFFSET - base_size
                ff = b"\xFF" * min(COPY_CHUNK, max(1, pad))
                while pad:
                    n = min(len(ff), pad)
                    dst.write(ff[:n])
                    pad -= n
                with dependency.open("rb") as dep:
                    shutil.copyfileobj(dep, dst, COPY_CHUNK)
                dst.flush()
                os.fsync(dst.fileno())
            os.replace(tmp, uze)
        finally:
            if tmp.exists():
                tmp.unlink()

    expected_size = DEPENDENCY_OFFSET + dep_size
    if uze.stat().st_size != expected_size:
        fail(f"packed size mismatch: got {uze.stat().st_size}, expected {expected_size}")
    with uze.open("rb") as packed:
        packed.seek(DEPENDENCY_LENGTH_OFFSET)
        stored_len = struct.unpack("<I", packed.read(4))[0]
        packed.seek(DEPENDENCY_OFFSET)
        magic = packed.read(4)
    if stored_len != dep_size:
        fail(f"dependency length mismatch: header={stored_len}, file={dep_size}")
    if magic != b"LIDX":
        fail(f"packed database is missing at offset {DEPENDENCY_OFFSET}")

    print(
        f"Packed database: {dep_size} bytes; verified LIDX at offset {DEPENDENCY_OFFSET}; "
        f"total UZE size {expected_size} bytes"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
