#!/usr/bin/env python3
from pathlib import Path
import argparse, hashlib, math, struct, zlib

def crc32_zeroed(buf: bytearray, off: int, size: int = 4) -> int:
    tmp = bytearray(buf)
    tmp[off:off+size] = b'\0'*size
    return zlib.crc32(tmp) & 0xffffffff

def put_cstr(dst: bytearray, off: int, size: int, text: str):
    b = text.encode('ascii', 'replace')[:size-1]
    dst[off:off+size] = b + b'\0'*(size-len(b))

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('database', type=Path)
    ap.add_argument('sid', type=Path)
    ap.add_argument('uzsd', type=Path)
    ap.add_argument('output', type=Path)
    args=ap.parse_args()
    db=bytearray(args.database.read_bytes())
    sid=args.sid.read_bytes(); stream=args.uzsd.read_bytes()
    if stream[:4] != b'UZSD' or stream[4] not in (3, 4):
        raise SystemExit('unexpected UZSD stream')
    packed_flags=stream[7]
    tick_hz=stream[5] | ((packed_flags >> 3) << 8)
    subtune=stream[6]; flags=packed_flags & 0x07
    length_ms,total_ticks,data_size=struct.unpack_from('<III',stream,12)
    if 49+data_size != len(stream):
        raise SystemExit(f'UZSD size mismatch header={data_size} actual={len(stream)-49}')
    if db[:4] != b'LIDX': raise SystemExit('missing LIDX')
    lengths_off=struct.unpack_from('<I',db,28)[0]
    lengths_size=struct.unpack_from('<I',db,32)[0]
    usdc_base=(lengths_off+lengths_size+511)&~511
    if db[usdc_base:usdc_base+4] != b'USDC': raise SystemExit('missing USDC')
    h=bytearray(db[usdc_base:usdc_base+64])
    block_shift=h[5]; block_size=1<<block_shift
    dir_count,dir_off,bitmap_off,data_off,total_blocks=struct.unpack_from('<IIIII',h,16)
    bitmap_size=(total_blocks+7)//8
    blocks=math.ceil(len(stream)/block_size)
    bitmap=bytearray(db[usdc_base+bitmap_off:usdc_base+bitmap_off+bitmap_size])
    first=None
    run=0
    for b in range(total_blocks):
        used=(bitmap[b>>3]>>(b&7))&1
        if not used:
            run += 1
            if run == blocks:
                first=b-blocks+1; break
        else: run=0
    if first is None: raise SystemExit('no free block run')
    entry_index=None
    for i in range(dir_count):
        if db[usdc_base+dir_off+i*128] in (0,3): entry_index=i; break
    if entry_index is None: raise SystemExit('no free directory entry')
    data_abs=usdc_base+data_off+first*block_size
    db[data_abs:data_abs+len(stream)] = stream
    # Clear block padding to deterministic zero.
    alloc_bytes=blocks*block_size
    db[data_abs+len(stream):data_abs+alloc_bytes]=b'\0'*(alloc_bytes-len(stream))
    for b in range(first,first+blocks): bitmap[b>>3] |= 1<<(b&7)
    db[usdc_base+bitmap_off:usdc_base+bitmap_off+bitmap_size]=bitmap
    md5=hashlib.md5(sid).digest()
    # PSID metadata
    title=sid[0x16:0x36].split(b'\0',1)[0].decode('latin1','replace').strip() or 'COMMANDO'
    author=sid[0x36:0x56].split(b'\0',1)[0].decode('latin1','replace').strip()
    released=sid[0x56:0x76].split(b'\0',1)[0].decode('latin1','replace').strip()
    subtunes=struct.unpack_from('>H',sid,0x0e)[0]
    e=bytearray(128)
    e[0]=1; e[1]=1
    struct.pack_into('<H',e,2,flags)
    e[4:20]=md5
    struct.pack_into('<IIHHHHII',e,20,len(stream),length_ms,subtune,subtunes,tick_hz,0,first,blocks)
    put_cstr(e,44,32,title)
    put_cstr(e,76,32,author)
    put_cstr(e,108,16,released)
    struct.pack_into('<I',e,124,crc32_zeroed(e,124))
    eabs=usdc_base+dir_off+entry_index*128
    db[eabs:eabs+128]=e
    used_blocks=struct.unpack_from('<I',h,36)[0]+blocks
    live_entries=struct.unpack_from('<I',h,44)[0]+1
    struct.pack_into('<III',h,36,used_blocks,first+blocks,live_entries)
    struct.pack_into('<I',h,48,0)
    struct.pack_into('<I',h,48,zlib.crc32(h)&0xffffffff)
    db[usdc_base:usdc_base+64]=h
    args.output.write_bytes(db)
    print(f'usdc_base={usdc_base}')
    print(f'entry_index={entry_index}')
    print(f'md5={md5.hex()}')
    print(f'subtune={subtune} subtunes={subtunes}')
    print(f'stream_size={len(stream)} length_ms={length_ms} tick_hz={tick_hz}')
    print(f'first_block={first} block_count={blocks}')
    print(f'title={title!r} author={author!r} released={released!r}')
    print(f'output={args.output} size={len(db)}')
if __name__=='__main__': main()
