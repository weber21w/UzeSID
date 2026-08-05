#!/usr/bin/env python3
import argparse
import re
import struct
import sys
import time
import zlib
from pathlib import Path

LIDX_HEADER_SIZE = 64
LIDX_RECORD_SIZE = 24
USDC_HEADER_SIZE = 64
USDC_ENTRY_SIZE = 128
USDC_FLAG_FIXED_METADATA_STRINGS = 0x00000001
USDC_FLAG_MD5_PRIMARY_KEY = 0x00000002
USDC_FLAG_BITMAP_ALLOCATOR = 0x00000004
USDC_FLAG_ENTRY_CRC32 = 0x00000008

LINE_RE = re.compile(r'^([0-9A-Fa-f]{32})=(.+)$')


def parse_size(text: str) -> int:
    s = text.strip()
    if s == '':
        raise ValueError('empty size')
    if s == '0':
        return 0
    m = re.fullmatch(r'(?i)(\d+)([KMG]i?B?|B)?', s)
    if not m:
        raise ValueError(f'bad size: {text}')
    n = int(m.group(1))
    suf = (m.group(2) or '').upper()
    mult = 1
    if suf in ('K', 'KB', 'KIB'):
        mult = 1024
    elif suf in ('M', 'MB', 'MIB'):
        mult = 1024 * 1024
    elif suf in ('G', 'GB', 'GIB'):
        mult = 1024 * 1024 * 1024
    elif suf in ('', 'B'):
        mult = 1
    else:
        raise ValueError(f'bad size suffix: {text}')
    return n * mult


def align_up(v: int, a: int) -> int:
    return (v + (a - 1)) & ~(a - 1)


def parse_time_ms(token: str) -> int:
    token = token.strip()
    if not token:
        raise ValueError('empty time token')

    ms = 0
    if '.' in token:
        main, frac = token.split('.', 1)
        frac = (frac + '000')[:3]
        ms = int(frac)
    else:
        main = token

    parts = main.split(':')
    if len(parts) == 2:
        minutes = int(parts[0])
        seconds = int(parts[1])
        total_ms = ((minutes * 60) + seconds) * 1000 + ms
    elif len(parts) == 3:
        hours = int(parts[0])
        minutes = int(parts[1])
        seconds = int(parts[2])
        total_ms = (((hours * 60 + minutes) * 60) + seconds) * 1000 + ms
    else:
        raise ValueError(f'bad time token: {token}')

    if total_ms < 0 or total_ms > 0xFFFFFF:
        raise ValueError(f'time out of 24-bit ms range: {token} -> {total_ms}')
    return total_ms


def build_lidx_payload(infile: Path, bucket_bits: int):
    if bucket_bits < 1 or bucket_bits > 16:
        raise ValueError('bucket_bits must be between 1 and 16')

    bucket_count = 1 << bucket_bits
    records = []
    seen = set()
    line_no = 0

    with infile.open('rt', encoding='utf-8', errors='replace') as f:
        for raw in f:
            line_no += 1
            line = raw.strip()
            if not line or line.startswith(';') or line.startswith('['):
                continue
            m = LINE_RE.match(line)
            if not m:
                continue
            md5_hex = m.group(1).lower()
            rhs = m.group(2).strip()
            tokens = rhs.split()
            if not tokens:
                continue

            md5_bytes = bytes.fromhex(md5_hex)
            lengths = [parse_time_ms(tok) for tok in tokens]
            if md5_bytes in seen:
                raise SystemExit(f'duplicate md5 at line {line_no}: {md5_hex}')
            seen.add(md5_bytes)
            records.append((md5_bytes, lengths))

    records.sort(key=lambda x: x[0])
    record_count = len(records)

    bucket_starts = [0] * (bucket_count + 1)
    rec_i = 0
    for b in range(bucket_count):
        while rec_i < record_count:
            md5 = records[rec_i][0]
            prefix = int.from_bytes(md5[:2], 'big') >> (16 - bucket_bits)
            if prefix >= b:
                break
            rec_i += 1
        bucket_starts[b] = rec_i
    bucket_starts[bucket_count] = record_count

    last = bucket_starts[bucket_count]
    for i in range(bucket_count - 1, -1, -1):
        if bucket_starts[i] == 0 and i != 0:
            bucket_starts[i] = last
        else:
            last = bucket_starts[i]
    first_nonzero = 0
    while first_nonzero < bucket_count and bucket_starts[first_nonzero] == 0:
        first_nonzero += 1
    if 0 < first_nonzero < bucket_count:
        for i in range(first_nonzero):
            bucket_starts[i] = bucket_starts[first_nonzero]

    bucket_table_offset = LIDX_HEADER_SIZE
    record_table_offset = bucket_table_offset + (bucket_count + 1) * 4
    lengths_blob_offset = record_table_offset + record_count * LIDX_RECORD_SIZE

    record_blob = bytearray()
    lengths_blob = bytearray()
    single_count = 0
    multi_count = 0
    max_subtunes = 0

    for md5_bytes, lengths in records:
        subtune_count = len(lengths)
        max_subtunes = max(max_subtunes, subtune_count)
        if subtune_count == 1:
            flags = 0x0001
            data_or_length = lengths[0]
            single_count += 1
        else:
            flags = 0
            data_or_length = len(lengths_blob)
            for value in lengths:
                lengths_blob += bytes((value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF))
            multi_count += 1
        record_blob += md5_bytes
        record_blob += struct.pack('<HHI', subtune_count, flags, data_or_length)

    bucket_blob = bytearray()
    for v in bucket_starts:
        bucket_blob += struct.pack('<I', v)

    flags = 0x00000003
    build_unix_time = int(time.time())
    reserved = bytes(16)
    payload = bucket_blob + record_blob + lengths_blob
    crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        '<4sBBHIIIIIIIIII16s',
        b'LIDX',
        1,
        bucket_bits,
        LIDX_HEADER_SIZE,
        flags,
        bucket_count,
        record_count,
        bucket_table_offset,
        record_table_offset,
        lengths_blob_offset,
        len(lengths_blob),
        LIDX_RECORD_SIZE,
        build_unix_time,
        crc32,
        reserved,
    )
    assert len(header) == LIDX_HEADER_SIZE

    return {
        'blob': header + payload,
        'record_count': record_count,
        'single_count': single_count,
        'multi_count': multi_count,
        'max_subtunes': max_subtunes,
        'bucket_count': bucket_count,
        'length_blob_size': len(lengths_blob),
        'crc32': crc32,
    }


def build_usdc_tail(cache_bytes: int, dir_entry_count: int, block_shift: int):
    if cache_bytes == 0:
        return b'', None
    if cache_bytes < USDC_HEADER_SIZE + (dir_entry_count * USDC_ENTRY_SIZE):
        raise ValueError('cache tail too small for header + directory')

    block_size = 1 << block_shift
    dir_offset = USDC_HEADER_SIZE
    bitmap_offset = dir_offset + dir_entry_count * USDC_ENTRY_SIZE

    # total_blocks depends on data_offset, and data_offset depends on bitmap size.
    # Iterate to a fixed point; this converges quickly.
    total_blocks = max(0, (cache_bytes - align_up(bitmap_offset, block_size)) // block_size)
    while True:
        bitmap_size = (total_blocks + 7) >> 3
        data_offset = align_up(bitmap_offset + bitmap_size, block_size)
        new_total_blocks = max(0, (cache_bytes - data_offset) // block_size)
        if new_total_blocks == total_blocks:
            break
        total_blocks = new_total_blocks

    bitmap_size = (total_blocks + 7) >> 3
    data_offset = align_up(bitmap_offset + bitmap_size, block_size)
    if total_blocks <= 0:
        raise ValueError('cache tail too small after layout to hold any data blocks')

    flags = (
        USDC_FLAG_FIXED_METADATA_STRINGS |
        USDC_FLAG_MD5_PRIMARY_KEY |
        USDC_FLAG_BITMAP_ALLOCATOR |
        USDC_FLAG_ENTRY_CRC32
    )

    reserved = bytes(12)
    raw = bytearray(64)
    struct.pack_into(
        '<4sBBHIIIIIIIIIII12s',
        raw,
        0,
        b'USDC',
        1,
        block_shift,
        USDC_ENTRY_SIZE,
        flags,
        cache_bytes,
        dir_entry_count,
        dir_offset,
        bitmap_offset,
        data_offset,
        total_blocks,
        0,
        0,
        0,
        0,
        reserved,
    )
    hdr_crc = zlib.crc32(raw[:48] + b'\x00\x00\x00\x00' + raw[52:]) & 0xFFFFFFFF
    struct.pack_into('<I', raw, 48, hdr_crc)

    tail = bytearray(cache_bytes)
    tail[0:64] = raw
    # directory / bitmap / data area already zeroed

    info = {
        'cache_bytes': cache_bytes,
        'dir_entry_count': dir_entry_count,
        'block_shift': block_shift,
        'block_size': block_size,
        'bitmap_offset': bitmap_offset,
        'bitmap_size': bitmap_size,
        'data_offset': data_offset,
        'total_blocks': total_blocks,
        'header_crc32': hdr_crc,
    }
    return bytes(tail), info


def main(argv=None):
    ap = argparse.ArgumentParser(description='Build UzeSID appended DB: LIDX + initialized USDC cache tail')
    ap.add_argument('songlengths', help='input Songlengths.md5')
    ap.add_argument('output', help='output appended DB binary')
    ap.add_argument('--bucket-bits', type=int, default=12, help='LIDX bucket bits, default 12')
    ap.add_argument('--tail-size', default='128M', help='USDC cache tail size, default 128M; use 0 for LIDX only')
    ap.add_argument('--dir-entries', type=int, default=512, help='USDC directory entry count, default 512')
    ap.add_argument('--block-shift', type=int, default=12, help='USDC block size as log2(bytes), default 12 => 4096')
    args = ap.parse_args(argv)

    infile = Path(args.songlengths)
    outfile = Path(args.output)
    tail_size = parse_size(args.tail_size)
    if args.block_shift < 9 or args.block_shift > 16:
        raise SystemExit('block_shift must be between 9 and 16')
    if args.dir_entries <= 0:
        raise SystemExit('dir_entries must be positive')

    lidx = build_lidx_payload(infile, args.bucket_bits)
    lidx_blob = lidx['blob']
    lidx_padded = align_up(len(lidx_blob), 512)
    pad_len = lidx_padded - len(lidx_blob)
    usdc_blob, usdc = build_usdc_tail(tail_size, args.dir_entries, args.block_shift)

    with outfile.open('wb') as f:
        f.write(lidx_blob)
        if pad_len:
            f.write(b'\x00' * pad_len)
        if usdc_blob:
            f.write(usdc_blob)

    print(f'input           : {infile}')
    print(f'output          : {outfile}')
    print(f'records         : {lidx["record_count"]}')
    print(f'single          : {lidx["single_count"]}')
    print(f'multi           : {lidx["multi_count"]}')
    print(f'max_subtunes    : {lidx["max_subtunes"]}')
    print(f'bucket_bits     : {args.bucket_bits}')
    print(f'bucket_count    : {lidx["bucket_count"]}')
    print(f'lidx_size       : {len(lidx_blob)}')
    print(f'lidx_padded     : {lidx_padded}')
    print(f'lidx_crc32      : {lidx["crc32"]:08x}')
    if usdc is None:
        print('usdc_tail       : disabled')
        print(f'total_size      : {lidx_padded}')
    else:
        print(f'usdc_tail       : {usdc["cache_bytes"]}')
        print(f'usdc_block_size : {usdc["block_size"]}')
        print(f'usdc_dir_entries: {usdc["dir_entry_count"]}')
        print(f'usdc_bitmap_off : {usdc["bitmap_offset"]}')
        print(f'usdc_data_off   : {usdc["data_offset"]}')
        print(f'usdc_blocks     : {usdc["total_blocks"]}')
        print(f'usdc_crc32      : {usdc["header_crc32"]:08x}')
        print(f'total_size      : {lidx_padded + usdc["cache_bytes"]}')


if __name__ == '__main__':
    main()
