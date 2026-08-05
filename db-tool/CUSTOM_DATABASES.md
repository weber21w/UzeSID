# Building writable UzeSID databases

`build_custom_database.py` converts PSID files into cached UZSD register streams and packs them into the fixed-size LIDX/USDC database appended to `UzeSID.uze`.

The converter is hybrid:

- Native C runs the same 6510 interpreter and ordered SID-register capture used by the ROM.
- Python discovers files, reads song lengths, runs captures in parallel, lays out the database, writes metadata/CRCs, and validates every embedded stream.

## Default project build

Put any `.sid` files to distribute in:

```text
db-tool/sids/
```

A normal build from `default/` automatically converts every SID in that directory, generates `db-tool/Songlengths_writable.bin`, regenerates `prebuilt_sids.inc`, and then explicitly appends and verifies the database in the `.uze` file:

```sh
make clean
make
```

The build uses `pack_uze_dependency.py` after `packrom`. It pads the program area to 60 KiB, writes the dependency length into the UZE header, appends the database at byte 61,952, and verifies the `LIDX` signature and final size. This avoids dependency-path newline handling differences between `packrom` versions.

The default USDC tail is **128 MiB** with 512 directory entries. The generated file is fixed-size. UzeSID never resizes it; runtime caching fails cleanly with `CACHE DATABASE FULL` or `CACHE DIRECTORY FULL` when allocation is no longer possible.

Change the fixed capacity without changing `gameinfo.properties`:

```sh
make clean-db
make DB_TAIL_SIZE=64M
```

Other examples:

```sh
make clean-db
make DB_TAIL_SIZE=256M DB_DIR_ENTRIES=1024 DB_JOBS=8
```

The on-disk format and runtime use 32-bit offsets, so sizes below 4 GiB are representable. 64–256 MiB is the practical range. FAT free space and the `.uze` packer must also accommodate the result.

`make clean` does not delete the database. It is rebuilt only when a SID, converter source, database script, or `Songlengths.md5`, `Songlengths.local.md5`, or `Songlengths.override.md5` changes. Use `make clean-db` to force regeneration.

## Song lengths

The builder hashes the complete SID file with MD5 and checks duration sources in this order for each subtune:

1. `db-tool/Songlengths.override.md5` — optional local corrections that intentionally supersede the official value.
2. `db-tool/Songlengths.md5` — the bundled official song-length database.
3. `db-tool/Songlengths.local.md5` — optional project-local fallback values used only when the official table has no value.
4. `--default-length` — three minutes by default.

Both local files use the normal syntax:

```text
0123456789abcdef0123456789abcdef=1:23 0:47.500
```

Use `Songlengths.local.md5` for SIDs absent from the official database. Use `Songlengths.override.md5` only after measuring that an official duration is unsuitable, such as a known earlier loop point.

When all tables lack a SID or subtune, the default is three minutes. Override it with:

```sh
python3 db-tool/build_custom_database.py --default-length 5:00
```

Use `--default-length none` to reject missing lengths.

The builder merges fallback durations into the generated LIDX, so the ROM sees the same lengths used during conversion.

## Standalone use

With no positional inputs, the tool scans `db-tool/sids` recursively and writes the default 128 MiB database:

```sh
python3 db-tool/build_custom_database.py
```

Equivalent explicit command:

```sh
python3 db-tool/build_custom_database.py db-tool/sids \
  --output db-tool/Songlengths_writable.bin \
  --tail-size 128M \
  --dir-entries 512 \
  --prebuilt-header db-tool/prebuilt_sids.inc
```

For a compact read-mostly distribution, request automatic sizing:

```sh
python3 db-tool/build_custom_database.py db-tool/sids \
  --output db-tool/Compact.bin \
  --tail-size auto \
  --reserve 1M
```

The builder refuses to create a fixed database when the converted streams do not fit. Runtime writes likewise allocate only within the existing bitmap and never extend the file.

## Output

The build produces:

- `Songlengths_writable.bin`
- `Songlengths_writable.bin.manifest.json`
- `prebuilt_sids.inc`

The manifest reports fixed tail size, block capacity, used blocks, and writable free bytes. Every database is reopened and all embedded UZSD data is compared byte-for-byte before the command succeeds.

Bundled database entries appear in UzeSID's merged browser even when the original `.SID` files are not copied beside the ROM. Put additional loose `.SID` files on the SD-card root when raw-file access is desired.


## SPI RAM stream capacity

The default build targets 64 SPI RAM banks (`spiram=64`), or 4 MiB.
The first bank plus 2 KiB are reserved for the emulated C64 and browser scratch
state, leaving 4,126,720 bytes for one temporary UZSD stream. The database
builder uses the same default through `--spi-banks 64`; override it with
`DB_SPI_BANKS=<count>` or the Python option when building for smaller targets.
The AVR uses 8-bit bank numbers and 32-bit linear offsets, so banks 0–63 are
addressable without changing the database format.

When a SID MD5 is absent from `Songlengths.md5`, the builder uses the configured
`--default-length` (three minutes by default). Build output labels each SID as
`lengths=Songlengths.md5`, `lengths=mixed/default`, or `lengths=default`.

### UZSD v4 compaction

`build_custom_database.py` first captures an exact ordered UZSD v3 stream with
the native converter, then rewrites it as compact UZSD v4.  The conversion is
lossless: register order, repeated gate/test transitions, empty ticks, clock,
and duration are preserved.  The manifest records both `raw_stream_size` and
the final `stream_size`.

The host converter allows up to 255 ordered SID writes in one replay tick.  If a
capture still stops, the error now identifies either `ordered-write limit` or
`SPI capacity`; increasing SPI banks cannot correct an ordered-write overflow.
