#!/usr/bin/env python3
"""Build a distributable UzeSID database from one or more PSID files.

The expensive 6510 pre-emulation is performed by the native C converter in
``db-tool/host-converter``.  Python handles discovery, song-length selection,
parallel process orchestration, LIDX/USDC construction, metadata, CRCs, and a
small optional AVR filename/MD5 lookup header.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass, asdict, replace
from pathlib import Path
from typing import Iterable

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
CONVERTER_DIR = SCRIPT_DIR / "host-converter"
CONVERTER = CONVERTER_DIR / "uzesid_capture"
BUILD_LIDX = SCRIPT_DIR / "build_lidx.py"
DEFAULT_SONGLENGTHS = SCRIPT_DIR / "Songlengths.md5"
DEFAULT_AUX_SONGLENGTHS = SCRIPT_DIR / "Songlengths.local.md5"
DEFAULT_OVERRIDE_SONGLENGTHS = SCRIPT_DIR / "Songlengths.override.md5"

LIDX_HEADER = struct.Struct("<4sBBHIIIIIIIIII16s")
UZSD_HEADER_SIZE = 49
UZSD_VERSION_V3 = 3
UZSD_VERSION = 4
USDC_HEADER_SIZE = 64
USDC_ENTRY_SIZE = 128
DEFAULT_BLOCK_SHIFT = 12
DEFAULT_DIR_ENTRIES = 512
DEFAULT_SPI_BANKS = 64
SPI_BANK_SIZE = 65536
SPI_STREAM_OFFSET = 0x10800
LINE_RE = re.compile(r"^([0-9a-fA-F]{32})=(.*)$")
SIZE_RE = re.compile(r"(?i)^(\d+)([KMG]i?B?|B)?$")

# UZSD v4 fixed signed-delta dictionary.  This must match Cache.c exactly.
UZSD_V4_DELTAS: tuple[tuple[int, ...], ...] = (
    (-6,6,8,-8,70,55,-55), (-1,-5,-4,1,-2,-6,-8),
    (32,-128,79,-32,64,40,3), (1,-1,-112,-111,32,-48,-15),
    (-1,-64,1,-112,57,-56,-57), (-5,5,15,-15,8,-8,72),
    (-54,54,-16,16,122,-122,-87), (70,-21,21,-70,110,-110,19),
    (-2,-1,3,-3,1,-4,22), (32,64,31,-17,-128,-96,8),
    (1,-1,-15,15,48,80,-2), (-1,-64,-56,1,-16,73,64),
    (15,-15,-6,6,-9,5,-5), (16,-16,75,-75,15,-118,103),
    (-126,126,-88,-125,-30,88,26), (-1,-2,119,1,-7,-119,16),
    (-128,22,16,111,8,32,-31), (16,1,-1,-16,-128,126,-2),
    (-1,1,-64,112,-112,64,-56), (9,-9,20,-20,8,-8,4),
    (-97,97,-39,39,35,-35,9), (0,0,0,0,0,0,0),
    (-1,-13,-24,-2,-3,-127,4), (-48,-32,-112,112,-16,2,-2),
    (48,-48,32,-32,-1,16,96),
)
UZSD_V4_END_TICK = 25
UZSD_V4_SKIP = 26
UZSD_V4_END = 27
UZSD_FLAG_TRUNCATED = 0x01
UZSD_FLAG_EVENT_OVERFLOW = 0x02
UZSD_FLAG_CAPACITY = 0x04


@dataclass(frozen=True)
class SidInfo:
    path: Path
    filename: str
    digest_hex: str
    songs: int
    title: str
    author: str
    released: str
    lengths_ms: tuple[int, ...]
    length_source: str


@dataclass(frozen=True)
class StreamInfo:
    sid_path: Path
    filename: str
    digest_hex: str
    songs: int
    title: str
    author: str
    released: str
    subtune: int
    requested_ms: int
    actual_ms: int
    tick_hz: int
    flags: int
    clock_hz: int
    total_ticks: int
    data_size: int
    stream_path: Path
    stream_size: int
    raw_stream_size: int
    length_source: str
    spi_banks: int


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def parse_size(text: str) -> int:
    value = text.strip()
    if value.lower() == "auto":
        return -1
    match = SIZE_RE.fullmatch(value)
    if not match:
        raise argparse.ArgumentTypeError(f"invalid size: {text}")
    number = int(match.group(1))
    suffix = (match.group(2) or "").upper()
    multipliers = {
        "": 1,
        "B": 1,
        "K": 1024,
        "KB": 1024,
        "KIB": 1024,
        "M": 1024 * 1024,
        "MB": 1024 * 1024,
        "MIB": 1024 * 1024,
        "G": 1024 * 1024 * 1024,
        "GB": 1024 * 1024 * 1024,
        "GIB": 1024 * 1024 * 1024,
    }
    return number * multipliers[suffix]


def parse_time_ms(token: str) -> int:
    token = token.strip()
    if not token:
        raise ValueError("empty duration")
    main, dot, fraction = token.partition(".")
    parts = main.split(":")
    if len(parts) == 2:
        seconds = int(parts[0]) * 60 + int(parts[1])
    elif len(parts) == 3:
        seconds = (int(parts[0]) * 60 + int(parts[1])) * 60 + int(parts[2])
    else:
        raise ValueError(f"bad duration: {token}")
    millis = int((fraction + "000")[:3]) if dot else 0
    value = seconds * 1000 + millis
    if value <= 0 or value > 0xFFFFFF:
        raise ValueError(f"duration outside UzeSID 24-bit range: {token}")
    return value


def format_time_ms(value: int) -> str:
    seconds, millis = divmod(value, 1000)
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        base = f"{hours}:{minutes:02d}:{seconds:02d}"
    else:
        base = f"{minutes}:{seconds:02d}"
    return f"{base}.{millis:03d}" if millis else base


def parse_songlengths(path: Path) -> tuple[dict[str, list[int]], list[str]]:
    records: dict[str, list[int]] = {}
    passthrough: list[str] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE_RE.match(raw.strip())
        if match:
            records[match.group(1).lower()] = [
                parse_time_ms(token) for token in match.group(2).split()
            ]
        else:
            passthrough.append(raw)
    return records, passthrough


def text_field(data: bytes, offset: int, size: int, fallback: str = "") -> str:
    raw = data[offset : offset + size].split(b"\0", 1)[0]
    text = raw.decode("latin1", errors="replace").strip()
    return text or fallback


def inspect_sid(
    path: Path,
    official_lengths: dict[str, list[int]],
    auxiliary_lengths: dict[str, list[int]],
    override_lengths: dict[str, list[int]],
    default_ms: int | None,
) -> SidInfo:
    data = path.read_bytes()
    if len(data) < 0x76 or data[:4] != b"PSID":
        raise ValueError(f"{path}: only PSID files are currently supported")
    version = int.from_bytes(data[4:6], "big")
    if version not in (1, 2, 3, 4):
        raise ValueError(f"{path}: unsupported PSID version {version}")
    data_offset = int.from_bytes(data[6:8], "big")
    if data_offset < 0x76 or data_offset > len(data):
        raise ValueError(f"{path}: invalid PSID data offset {data_offset}")
    songs = int.from_bytes(data[0x0E:0x10], "big") or 1
    digest_hex = hashlib.md5(data).hexdigest()
    # Local overrides intentionally take precedence so known-wrong official
    # durations can be corrected without editing the large upstream file.
    # The auxiliary table is otherwise consulted only for missing official
    # records or missing subtune values.
    override = override_lengths.get(digest_hex, [])
    official = official_lengths.get(digest_hex, [])
    auxiliary = auxiliary_lengths.get(digest_hex, [])
    selected: list[int] = []
    sources: list[str] = []
    for subtune in range(songs):
        if subtune < len(override):
            selected.append(override[subtune])
            sources.append("Songlengths.override.md5")
        elif subtune < len(official):
            selected.append(official[subtune])
            sources.append("Songlengths.md5")
        elif subtune < len(auxiliary):
            selected.append(auxiliary[subtune])
            sources.append("Songlengths.local.md5")
        elif default_ms is not None:
            selected.append(default_ms)
            sources.append("default")
        else:
            raise ValueError(
                f"{path}: no duration for subtune {subtune + 1}; "
                "add it to Songlengths.local.md5 or supply --default-length"
            )
    unique_sources = list(dict.fromkeys(sources))
    source = unique_sources[0] if len(unique_sources) == 1 else "+".join(unique_sources)
    return SidInfo(
        path=path.resolve(),
        filename=path.name,
        digest_hex=digest_hex,
        songs=songs,
        title=text_field(data, 0x16, 32, path.stem),
        author=text_field(data, 0x36, 32),
        released=text_field(data, 0x56, 32),
        lengths_ms=tuple(selected),
        length_source=source,
    )


def discover_sids(inputs: Iterable[Path], recursive: bool) -> list[Path]:
    found: list[Path] = []
    for item in inputs:
        item = item.expanduser()
        if item.is_file():
            if item.suffix.lower() == ".sid":
                found.append(item)
            else:
                raise ValueError(f"not a .sid file: {item}")
        elif item.is_dir():
            iterator = item.rglob("*") if recursive else item.glob("*")
            found.extend(path for path in iterator if path.is_file() and path.suffix.lower() == ".sid")
        else:
            raise ValueError(f"input does not exist: {item}")
    unique: dict[Path, Path] = {}
    for path in found:
        unique[path.resolve()] = path.resolve()
    return sorted(unique.values(), key=lambda path: (path.name.lower(), str(path).lower()))


def ensure_converter(rebuild: bool) -> None:
    command = ["make", "-C", str(CONVERTER_DIR)]
    if rebuild:
        subprocess.run(command + ["clean"], check=True)
    subprocess.run(command, check=True)
    if not CONVERTER.is_file():
        raise RuntimeError(f"converter was not created: {CONVERTER}")


def safe_stream_name(info: SidInfo, subtune: int) -> str:
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", info.path.stem).strip("._") or "song"
    return f"{stem}_{info.digest_hex[:8]}_sub{subtune + 1:03d}.UZSD"


def read_uleb128(data: bytes, position: int) -> tuple[int, int]:
    value = 0
    shift = 0
    while True:
        if position >= len(data):
            raise ValueError("truncated ULEB128")
        byte = data[position]
        position += 1
        value |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return value, position
        shift += 7
        if shift > 28:
            raise ValueError("oversized ULEB128")


def append_uleb128(output: bytearray, value: int) -> None:
    if value <= 0:
        raise ValueError("UZSD skip run must be positive")
    while True:
        byte = value & 0x7F
        value >>= 7
        output.append(byte | (0x80 if value else 0))
        if not value:
            return


def iter_v3_ticks(data: bytes) -> Iterable[list[tuple[int, int]]]:
    position = UZSD_HEADER_SIZE
    end = position + struct.unpack_from("<I", data, 20)[0]
    while position < end:
        op = data[position]
        position += 1
        if op == 0xFF:
            return
        if op == 0x00:
            run, position = read_uleb128(data, position)
            for _ in range(max(1, run)):
                yield []
            continue
        if op == 0x01:
            if position >= end:
                raise ValueError("truncated UZSD pair count")
            count = data[position]
            position += 1
            byte_count = count * 2
            if position + byte_count > end:
                raise ValueError("truncated UZSD pairs")
            events = [(data[position + i * 2], data[position + i * 2 + 1]) for i in range(count)]
            position += byte_count
            yield events
            continue
        if op == 0x02:
            if position + 4 > end:
                raise ValueError("truncated UZSD mask")
            mask = data[position:position + 4]
            position += 4
            events: list[tuple[int, int]] = []
            for reg in range(25):
                if mask[reg >> 3] & (1 << (reg & 7)):
                    if position >= end:
                        raise ValueError("truncated UZSD mask values")
                    events.append((reg, data[position]))
                    position += 1
            yield events
            continue
        raise ValueError(f"unsupported UZSD v3 opcode 0x{op:02x}")
    raise ValueError("UZSD v3 stream has no end marker")


def compact_stream_v4(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < UZSD_HEADER_SIZE or data[:4] != b"UZSD":
        raise ValueError(f"{path}: invalid UZSD stream")
    if data[4] == UZSD_VERSION:
        return len(data)
    if data[4] != UZSD_VERSION_V3:
        raise ValueError(f"{path}: cannot compact UZSD version {data[4]}")

    previous = list(data[24:49])
    payload = bytearray()
    empty_run = 0
    decoded_ticks = 0
    for events in iter_v3_ticks(data):
        decoded_ticks += 1
        if not events:
            empty_run += 1
            continue
        if empty_run:
            payload.append(UZSD_V4_SKIP)
            append_uleb128(payload, empty_run)
            empty_run = 0
        for reg, value in events:
            if reg >= 25:
                raise ValueError(f"{path}: invalid SID register {reg}")
            delta = ((value - previous[reg] + 128) & 0xFF) - 128
            try:
                code = UZSD_V4_DELTAS[reg].index(delta)
            except ValueError:
                code = 7
            payload.append((code << 5) | reg)
            if code == 7:
                payload.append(value)
            previous[reg] = value
        payload.append(UZSD_V4_END_TICK)
    if empty_run:
        payload.append(UZSD_V4_SKIP)
        append_uleb128(payload, empty_run)
    payload.append(UZSD_V4_END)

    expected_ticks = struct.unpack_from("<I", data, 16)[0]
    if decoded_ticks != expected_ticks:
        raise ValueError(f"{path}: decoded {decoded_ticks} ticks, header declares {expected_ticks}")
    result = bytearray(data[:UZSD_HEADER_SIZE])
    result[4] = UZSD_VERSION
    struct.pack_into("<I", result, 20, len(payload))
    result.extend(payload)
    raw_size = len(data)
    path.write_bytes(result)
    return raw_size


def parse_stream(path: Path, sid: SidInfo, subtune: int, requested_ms: int, spi_banks: int) -> StreamInfo:
    data = path.read_bytes()
    if len(data) < UZSD_HEADER_SIZE or data[:4] != b"UZSD":
        raise ValueError(f"{path}: invalid UZSD header")
    version, tick_lo, encoded_subtune, packed_flags = data[4:8]
    tick_hz = tick_lo | ((packed_flags >> 3) << 8)
    flags = packed_flags & 0x07
    if version not in (UZSD_VERSION_V3, UZSD_VERSION):
        raise ValueError(f"{path}: unsupported UZSD version {version}")
    if encoded_subtune != subtune:
        raise ValueError(f"{path}: subtune mismatch {encoded_subtune} != {subtune}")
    clock_hz, actual_ms, total_ticks, data_size = struct.unpack_from("<IIII", data, 8)
    if UZSD_HEADER_SIZE + data_size != len(data):
        raise ValueError(
            f"{path}: payload size mismatch {data_size} != {len(data) - UZSD_HEADER_SIZE}"
        )
    return StreamInfo(
        sid_path=sid.path,
        filename=sid.filename,
        digest_hex=sid.digest_hex,
        songs=sid.songs,
        title=sid.title,
        author=sid.author,
        released=sid.released,
        subtune=subtune,
        requested_ms=requested_ms,
        actual_ms=actual_ms,
        tick_hz=tick_hz,
        flags=flags,
        clock_hz=clock_hz,
        total_ticks=total_ticks,
        data_size=data_size,
        stream_path=path,
        stream_size=len(data),
        raw_stream_size=len(data),
        length_source=sid.length_source,
        spi_banks=spi_banks,
    )


def capture_one(task: tuple[SidInfo, int, int, Path, int]) -> StreamInfo:
    sid, subtune, length_ms, output, spi_banks = task
    command = [
        str(CONVERTER), str(sid.path), str(subtune), str(length_ms),
        str(output), str(spi_banks),
    ]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            f"capture failed for {sid.filename} subtune {subtune + 1}/{sid.songs}: {detail}"
        )
    raw_size = compact_stream_v4(output)
    return replace(parse_stream(output, sid, subtune, length_ms, spi_banks), raw_stream_size=raw_size)


def crc_zeroed(buffer: bytes | bytearray, offset: int, size: int = 4) -> int:
    copy = bytearray(buffer)
    copy[offset : offset + size] = b"\0" * size
    return zlib.crc32(copy) & 0xFFFFFFFF


def put_cstr(buffer: bytearray, offset: int, size: int, text: str) -> None:
    encoded = text.encode("ascii", errors="replace")[: size - 1]
    buffer[offset : offset + size] = encoded + b"\0" * (size - len(encoded))


def usdc_layout(cache_bytes: int, dir_entries: int, block_shift: int) -> tuple[int, int, int]:
    block_size = 1 << block_shift
    bitmap_offset = USDC_HEADER_SIZE + dir_entries * USDC_ENTRY_SIZE
    total_blocks = max(0, (cache_bytes - align_up(bitmap_offset, block_size)) // block_size)
    while True:
        bitmap_size = (total_blocks + 7) // 8
        data_offset = align_up(bitmap_offset + bitmap_size, block_size)
        new_total = max(0, (cache_bytes - data_offset) // block_size)
        if new_total == total_blocks:
            return data_offset, total_blocks, total_blocks * block_size
        total_blocks = new_total


def automatic_tail_size(data_bytes: int, reserve_bytes: int, dir_entries: int, block_shift: int) -> int:
    required = data_bytes + reserve_bytes
    tail = align_up(USDC_HEADER_SIZE + dir_entries * USDC_ENTRY_SIZE + required + 65536, 512)
    while True:
        _, _, capacity = usdc_layout(tail, dir_entries, block_shift)
        if capacity >= required:
            return tail
        tail = align_up(tail + (required - capacity) + (1 << block_shift), 512)


def write_merged_songlengths(
    source_path: Path,
    source_records: dict[str, list[int]],
    sid_infos: list[SidInfo],
    output: Path,
) -> None:
    overrides = {info.digest_hex: list(info.lengths_ms) for info in sid_infos}
    lines: list[str] = []
    seen: set[str] = set()
    for raw in source_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE_RE.match(raw.strip())
        if not match:
            lines.append(raw)
            continue
        digest = match.group(1).lower()
        if digest in overrides:
            values = " ".join(format_time_ms(value) for value in overrides[digest])
            lines.append(f"{digest}={values}")
            seen.add(digest)
        else:
            lines.append(raw)
    for digest in sorted(overrides):
        if digest not in seen and digest not in source_records:
            values = " ".join(format_time_ms(value) for value in overrides[digest])
            lines.append(f"{digest}={values}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_empty_database(
    songlengths: Path,
    output: Path,
    tail_bytes: int,
    dir_entries: int,
    block_shift: int,
) -> None:
    command = [
        sys.executable,
        str(BUILD_LIDX),
        str(songlengths),
        str(output),
        "--tail-size",
        str(tail_bytes),
        "--dir-entries",
        str(dir_entries),
        "--block-shift",
        str(block_shift),
    ]
    subprocess.run(command, check=True)


def inject_streams(database: Path, output: Path, streams: list[StreamInfo]) -> dict[str, int]:
    blob = bytearray(database.read_bytes())
    if len(blob) < LIDX_HEADER.size or blob[:4] != b"LIDX":
        raise ValueError("generated database is missing LIDX")
    fields = LIDX_HEADER.unpack_from(blob, 0)
    lengths_offset, lengths_size = fields[9], fields[10]
    usdc_base = align_up(lengths_offset + lengths_size, 512)
    if blob[usdc_base : usdc_base + 4] != b"USDC":
        raise ValueError(f"generated database is missing USDC at {usdc_base}")

    header = bytearray(blob[usdc_base : usdc_base + USDC_HEADER_SIZE])
    block_shift = header[5]
    entry_size = struct.unpack_from("<H", header, 6)[0]
    if entry_size != USDC_ENTRY_SIZE:
        raise ValueError(f"unsupported USDC entry size {entry_size}")
    dir_count, dir_offset, bitmap_offset, data_offset, total_blocks = struct.unpack_from(
        "<IIIII", header, 16
    )
    block_size = 1 << block_shift
    bitmap_size = (total_blocks + 7) // 8
    bitmap = bytearray(blob[usdc_base + bitmap_offset : usdc_base + bitmap_offset + bitmap_size])

    if len(streams) > dir_count:
        raise ValueError(f"{len(streams)} streams exceed {dir_count} directory entries")

    next_block = 0
    used_blocks = 0
    for index, stream in enumerate(streams):
        data = stream.stream_path.read_bytes()
        blocks = math.ceil(len(data) / block_size)
        while next_block < total_blocks and (bitmap[next_block >> 3] >> (next_block & 7)) & 1:
            next_block += 1
        first_block = next_block
        if first_block + blocks > total_blocks:
            raise ValueError(
                f"USDC tail is full while adding {stream.filename} subtune {stream.subtune + 1}"
            )
        for block in range(first_block, first_block + blocks):
            if (bitmap[block >> 3] >> (block & 7)) & 1:
                raise ValueError("allocator encountered a fragmented block run")
        data_abs = usdc_base + data_offset + first_block * block_size
        alloc_size = blocks * block_size
        blob[data_abs : data_abs + len(data)] = data
        blob[data_abs + len(data) : data_abs + alloc_size] = b"\0" * (alloc_size - len(data))
        for block in range(first_block, first_block + blocks):
            bitmap[block >> 3] |= 1 << (block & 7)

        entry = bytearray(USDC_ENTRY_SIZE)
        entry[0] = 1  # LIVE
        entry[1] = 1  # UZSD
        struct.pack_into("<H", entry, 2, stream.flags)
        entry[4:20] = bytes.fromhex(stream.digest_hex)
        struct.pack_into(
            "<IIHHHHII",
            entry,
            20,
            stream.stream_size,
            stream.actual_ms,
            stream.subtune,
            stream.songs,
            stream.tick_hz,
            0,
            first_block,
            blocks,
        )
        put_cstr(entry, 44, 32, stream.title)
        put_cstr(entry, 76, 32, stream.author)
        put_cstr(entry, 108, 16, stream.released)
        struct.pack_into("<I", entry, 124, crc_zeroed(entry, 124))
        entry_abs = usdc_base + dir_offset + index * entry_size
        blob[entry_abs : entry_abs + entry_size] = entry

        next_block = first_block + blocks
        used_blocks += blocks

    blob[usdc_base + bitmap_offset : usdc_base + bitmap_offset + bitmap_size] = bitmap
    struct.pack_into("<III", header, 36, used_blocks, next_block, len(streams))
    struct.pack_into("<I", header, 48, 0)
    struct.pack_into("<I", header, 48, zlib.crc32(header) & 0xFFFFFFFF)
    blob[usdc_base : usdc_base + USDC_HEADER_SIZE] = header

    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_bytes(blob)
    temporary.replace(output)
    return {
        "usdc_base": usdc_base,
        "block_size": block_size,
        "total_blocks": total_blocks,
        "used_blocks": used_blocks,
        "free_blocks": total_blocks - used_blocks,
        "live_entries": len(streams),
        "database_bytes": len(blob),
    }


def c_escape(text: str) -> str:
    result = []
    for char in text:
        code = ord(char)
        if char in ('\\', '"'):
            result.append("\\" + char)
        elif 32 <= code <= 126:
            result.append(char)
        else:
            result.append("_")
    return "".join(result)


def write_prebuilt_header(path: Path, sid_infos: list[SidInfo]) -> None:
    lines = [
        "/* Generated by db-tool/build_custom_database.py. */",
        "#ifndef UZESID_PREBUILT_SIDS_INC",
        "#define UZESID_PREBUILT_SIDS_INC",
        f"#define UZESID_PREBUILT_COUNT {len(sid_infos)}u",
    ]
    if sid_infos:
        lines.append(
            "static const UzeSidPrebuiltInfo g_prebuilt_sids[UZESID_PREBUILT_COUNT] PROGMEM = {"
        )
        for info in sid_infos:
            digest = ",".join(f"0x{value:02x}" for value in bytes.fromhex(info.digest_hex))
            lines.append(
                f'    {{ "{c_escape(info.filename)}", {{ {digest} }}, {info.songs}u }},'
            )
        lines.append("};")
    lines.extend(["#endif", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def validate_database(output: Path, streams: list[StreamInfo]) -> None:
    blob = output.read_bytes()
    fields = LIDX_HEADER.unpack_from(blob, 0)
    usdc_base = align_up(fields[9] + fields[10], 512)
    header = bytearray(blob[usdc_base : usdc_base + USDC_HEADER_SIZE])
    expected_header_crc = struct.unpack_from("<I", header, 48)[0]
    if crc_zeroed(header, 48) != expected_header_crc:
        raise ValueError("USDC header CRC mismatch")
    block_shift = header[5]
    entry_size = struct.unpack_from("<H", header, 6)[0]
    dir_count, dir_offset, data_offset = (
        struct.unpack_from("<I", header, 16)[0],
        struct.unpack_from("<I", header, 20)[0],
        struct.unpack_from("<I", header, 28)[0],
    )
    block_size = 1 << block_shift
    found: dict[tuple[str, int], bytes] = {}
    for index in range(dir_count):
        offset = usdc_base + dir_offset + index * entry_size
        entry = bytearray(blob[offset : offset + entry_size])
        if len(entry) != entry_size or entry[0] != 1 or entry[1] != 1:
            continue
        expected_crc = struct.unpack_from("<I", entry, 124)[0]
        if crc_zeroed(entry, 124) != expected_crc:
            raise ValueError(f"USDC entry {index} CRC mismatch")
        digest = bytes(entry[4:20]).hex()
        size = struct.unpack_from("<I", entry, 20)[0]
        subtune = struct.unpack_from("<H", entry, 28)[0]
        first_block = struct.unpack_from("<I", entry, 36)[0]
        stream_offset = usdc_base + data_offset + first_block * block_size
        stream = bytes(blob[stream_offset : stream_offset + size])
        if len(stream) != size or stream[:4] != b"UZSD":
            raise ValueError(f"USDC entry {index} stream is invalid")
        found[(digest, subtune)] = stream
    for expected in streams:
        key = (expected.digest_hex, expected.subtune)
        actual = found.get(key)
        if actual is None:
            raise ValueError(f"missing cached entry {expected.filename} subtune {expected.subtune + 1}")
        if actual != expected.stream_path.read_bytes():
            raise ValueError(f"cached data mismatch for {expected.filename} subtune {expected.subtune + 1}")


def manifest_stream(stream: StreamInfo, retain_stream_path: bool) -> dict[str, object]:
    result = asdict(stream)
    result["sid_path"] = str(result["sid_path"])
    result["stream_path"] = str(result["stream_path"]) if retain_stream_path else None
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pre-emulate PSID subtunes in native C and build a distributable UzeSID database"
    )
    parser.add_argument(
        "inputs", nargs="*", type=Path,
        help=".sid files or directories; default db-tool/sids",
    )
    parser.add_argument(
        "-o", "--output", type=Path,
        default=SCRIPT_DIR / "Songlengths_writable.bin",
        help="output appended database binary; default db-tool/Songlengths_writable.bin",
    )
    parser.add_argument(
        "--songlengths", type=Path, default=DEFAULT_SONGLENGTHS, help="base Songlengths.md5"
    )
    parser.add_argument(
        "--aux-songlengths", type=Path, default=DEFAULT_AUX_SONGLENGTHS,
        help="optional fallback lengths checked after Songlengths.md5",
    )
    parser.add_argument(
        "--override-songlengths", type=Path, default=DEFAULT_OVERRIDE_SONGLENGTHS,
        help="optional local corrections checked before Songlengths.md5",
    )
    parser.add_argument(
        "--default-length",
        default="3:00",
        help="duration used for missing subtune lengths; use 'none' to fail",
    )
    parser.add_argument(
        "--tail-size",
        default="128M",
        help="fixed writable USDC tail size such as 64M or 128M; default 128M; use auto for compact output",
    )
    parser.add_argument(
        "--reserve",
        default="1M",
        help="extra free USDC data space when --tail-size=auto; ignored for fixed sizes",
    )
    parser.add_argument("--dir-entries", type=int, default=DEFAULT_DIR_ENTRIES)
    parser.add_argument("--block-shift", type=int, default=DEFAULT_BLOCK_SHIFT)
    parser.add_argument("-j", "--jobs", type=int, default=max(1, min(8, os.cpu_count() or 1)))
    parser.add_argument("--no-recursive", action="store_true", help="do not recurse into input directories")
    parser.add_argument(
        "--spi-banks", type=int, default=DEFAULT_SPI_BANKS,
        help="target SPI RAM bank count (64 KiB per bank); default 64 (4 MiB)",
    )
    parser.add_argument(
        "--allow-truncated", action="store_true",
        help="accept streams that fill the configured SPI RAM capacity",
    )
    parser.add_argument("--keep-streams", type=Path, help="retain generated .UZSD files in this directory")
    parser.add_argument("--manifest", type=Path, help="manifest JSON path; defaults beside output")
    parser.add_argument("--prebuilt-header", type=Path, help="generate AVR filename/MD5 lookup include")
    parser.add_argument("--copy-sids", type=Path, help="copy source SID files into this directory")
    parser.add_argument("--rebuild-converter", action="store_true")
    args = parser.parse_args(argv)

    started = time.monotonic()
    if args.spi_banks < 2 or args.spi_banks > 64:
        raise ValueError("--spi-banks must be between 2 and 64")
    stream_capacity = args.spi_banks * SPI_BANK_SIZE - SPI_STREAM_OFFSET
    input_paths = args.inputs or [SCRIPT_DIR / "sids"]
    sid_paths = discover_sids(input_paths, recursive=not args.no_recursive)
    if not sid_paths:
        raise ValueError("no .sid files found")
    if not args.songlengths.is_file():
        raise ValueError(f"song-length database not found: {args.songlengths}")
    source_lengths, _ = parse_songlengths(args.songlengths)
    auxiliary_lengths: dict[str, list[int]] = {}
    override_lengths: dict[str, list[int]] = {}
    if args.aux_songlengths.is_file():
        auxiliary_lengths, _ = parse_songlengths(args.aux_songlengths)
    if args.override_songlengths.is_file():
        override_lengths, _ = parse_songlengths(args.override_songlengths)
    default_ms = None if args.default_length.lower() == "none" else parse_time_ms(args.default_length)

    sid_infos: list[SidInfo] = []
    seen_digest: dict[str, Path] = {}
    seen_filename: dict[str, Path] = {}
    for path in sid_paths:
        info = inspect_sid(path, source_lengths, auxiliary_lengths, override_lengths, default_ms)
        previous = seen_digest.get(info.digest_hex)
        if previous is not None:
            print(f"Skipping duplicate SID content: {path} (same as {previous})")
            continue
        name_key = info.filename.casefold()
        previous_name = seen_filename.get(name_key)
        if previous_name is not None:
            raise ValueError(
                f"duplicate root filename {info.filename!r}: {previous_name} and {path}; "
                "rename one file before building"
            )
        if len(info.filename.encode("ascii", errors="replace")) >= 32:
            raise ValueError(f"filename is too long for the raw browser/prebuilt table: {info.filename}")
        seen_digest[info.digest_hex] = path
        seen_filename[name_key] = path
        sid_infos.append(info)

    stream_count = sum(info.songs for info in sid_infos)
    if stream_count > args.dir_entries:
        raise ValueError(
            f"{stream_count} subtunes exceed --dir-entries={args.dir_entries}; increase it"
        )
    print(f"Found {len(sid_infos)} unique SID files with {stream_count} subtunes")
    for info in sid_infos:
        print(
            f"  {info.filename}: {info.songs} subtune(s), {info.digest_hex}, lengths={info.length_source}"
        )

    ensure_converter(args.rebuild_converter)

    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    manifest_path = (args.manifest or args.output.with_suffix(args.output.suffix + ".manifest.json")).resolve()

    temporary_context = None
    if args.keep_streams:
        stream_dir = args.keep_streams.resolve()
        stream_dir.mkdir(parents=True, exist_ok=True)
    else:
        temporary_context = tempfile.TemporaryDirectory(prefix="uzesid-build-")
        stream_dir = Path(temporary_context.name)

    tasks: list[tuple[SidInfo, int, int, Path, int]] = []
    for info in sid_infos:
        for subtune, length_ms in enumerate(info.lengths_ms):
            tasks.append((info, subtune, length_ms, stream_dir / safe_stream_name(info, subtune), args.spi_banks))

    streams: list[StreamInfo] = []
    jobs = max(1, min(args.jobs, len(tasks)))
    print(f"Capturing {len(tasks)} stream(s) with {jobs} native worker(s)...")
    if jobs == 1:
        for number, task in enumerate(tasks, 1):
            result = capture_one(task)
            streams.append(result)
            print(
                f"  [{number}/{len(tasks)}] {result.filename} {result.subtune + 1}/{result.songs}: "
                f"{result.stream_size:,} bytes (raw {result.raw_stream_size:,})"
            )
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
            future_map = {executor.submit(capture_one, task): task for task in tasks}
            completed = 0
            for future in concurrent.futures.as_completed(future_map):
                result = future.result()
                streams.append(result)
                completed += 1
                print(
                    f"  [{completed}/{len(tasks)}] {result.filename} {result.subtune + 1}/{result.songs}: "
                    f"{result.stream_size:,} bytes (raw {result.raw_stream_size:,})"
                )
    streams.sort(key=lambda item: (item.filename.lower(), item.subtune))

    oversized = [stream for stream in streams if stream.stream_size > stream_capacity]
    if oversized:
        details = ", ".join(
            f"{item.filename}#{item.subtune + 1} "
            f"({item.stream_size:,} compacted bytes)" for item in oversized[:8]
        )
        raise ValueError(
            f"{len(oversized)} compacted stream(s) exceed {args.spi_banks} target SPI RAM "
            f"banks ({stream_capacity:,} bytes available): {details}"
        )

    truncated = [stream for stream in streams if stream.flags & UZSD_FLAG_TRUNCATED]
    if truncated and not args.allow_truncated:
        details = []
        for item in truncated[:8]:
            details.append(
                f"{item.filename}#{item.subtune + 1} "
                f"({format_time_ms(item.requested_ms)}, lengths={item.length_source})"
            )
        names = ", ".join(details)
        reasons = set()
        for item in truncated:
            if item.flags & UZSD_FLAG_EVENT_OVERFLOW:
                reasons.add("ordered-write limit")
            if item.flags & UZSD_FLAG_CAPACITY:
                reasons.add("SPI capacity")
        reason_text = ", ".join(sorted(reasons)) or "capture stopped early"
        raise ValueError(
            f"{len(truncated)} stream(s) were truncated ({reason_text}); target has "
            f"{args.spi_banks} SPI RAM banks ({stream_capacity:,} raw capture bytes): {names}"
        )

    used_data_bytes = sum(align_up(stream.stream_size, 1 << args.block_shift) for stream in streams)
    tail_arg = parse_size(args.tail_size)
    reserve_bytes = parse_size(args.reserve)
    if reserve_bytes < 0:
        raise ValueError("--reserve cannot be auto")
    if tail_arg < 0:
        tail_bytes = automatic_tail_size(
            used_data_bytes, reserve_bytes, args.dir_entries, args.block_shift
        )
    else:
        tail_bytes = tail_arg
    if tail_bytes <= 0:
        raise ValueError("--tail-size must leave a writable USDC region")
    if tail_bytes > 0xFFFFFFFF:
        raise ValueError("--tail-size exceeds the 32-bit UzeSID/Petit FatFs format limit")
    data_offset, total_blocks, data_capacity = usdc_layout(
        tail_bytes, args.dir_entries, args.block_shift
    )
    if used_data_bytes > data_capacity:
        raise ValueError(
            f"USDC tail is too small: streams need {used_data_bytes:,} bytes but "
            f"only {data_capacity:,} bytes are allocatable"
        )
    free_after_build = data_capacity - used_data_bytes
    print(
        f"Stream allocation: {used_data_bytes:,} bytes; USDC tail: {tail_bytes:,} bytes; "
        f"writable free space after build: {free_after_build:,} bytes"
    )

    with tempfile.TemporaryDirectory(prefix="uzesid-db-") as db_temp_name:
        db_temp = Path(db_temp_name)
        merged_lengths = db_temp / "Songlengths.generated.md5"
        empty_database = db_temp / "database.empty.bin"
        write_merged_songlengths(args.songlengths, source_lengths, sid_infos, merged_lengths)
        build_empty_database(
            merged_lengths,
            empty_database,
            tail_bytes,
            args.dir_entries,
            args.block_shift,
        )
        database_info = inject_streams(empty_database, args.output, streams)

    validate_database(args.output, streams)

    header_path = args.prebuilt_header.resolve() if args.prebuilt_header else None
    if header_path:
        write_prebuilt_header(header_path, sid_infos)

    if args.copy_sids:
        copy_dir = args.copy_sids.resolve()
        copy_dir.mkdir(parents=True, exist_ok=True)
        for info in sid_infos:
            shutil.copy2(info.path, copy_dir / info.filename)

    manifest = {
        "format": "UzeSID custom database manifest v1",
        "created_unix": int(time.time()),
        "output": str(args.output),
        "songlengths": str(args.songlengths.resolve()),
        "aux_songlengths": str(args.aux_songlengths.resolve()) if args.aux_songlengths.is_file() else None,
        "override_songlengths": str(args.override_songlengths.resolve()) if args.override_songlengths.is_file() else None,
        "sid_files": [
            {
                **asdict(info),
                "path": str(info.path),
                "lengths_ms": list(info.lengths_ms),
            }
            for info in sid_infos
        ],
        "streams": [manifest_stream(stream, args.keep_streams is not None) for stream in streams],
        "database": database_info,
        "tail_bytes": tail_bytes,
        "usdc_data_offset": data_offset,
        "usdc_total_blocks": total_blocks,
        "writable_free_bytes": free_after_build,
        "truncated_streams": len(truncated),
        "spi_banks": args.spi_banks,
        "spi_stream_capacity": stream_capacity,
        "prebuilt_header": str(header_path) if header_path else None,
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    elapsed = time.monotonic() - started
    print(f"Validated {len(streams)} cached stream(s)")
    print(f"Database: {args.output} ({args.output.stat().st_size:,} bytes)")
    print(f"Manifest: {manifest_path}")
    if header_path:
        print(f"Prebuilt lookup: {header_path}")
    print(f"Completed in {elapsed:.2f} seconds")

    if temporary_context is not None:
        temporary_context.cleanup()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"build failed: {error}", file=sys.stderr)
        raise SystemExit(1)
