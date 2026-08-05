# UzeSID

Commodore 64 SID music player for Uzebox.

UzeSID plays supported PSID music files using a lightweight 6502 and SID emulation system designed for the ATmega644. SID files are pre-emulated into compact, ordered streams of SID-register writes and then played by an optimized SID synthesizer.

Native 15.72 kHz synthesis is the default. An optional 7.86 kHz synthesis mode with interpolation is available for additional CPU margin.


## Demo Video

Thanks to [creator name] for testing UzeSID and recording this demonstration:

[![Watch the UzeSID demo](https://img.youtube.com/vi/rDpJzCZ6fUg/0.jpg)](https://www.youtube.com/watch?v=rDpJzCZ6fUg)


## Overview

UzeSID can play music from:

* `.SID` files stored in the root directory of an SD card
* Pre-converted songs embedded in the database appended to `UZESID.UZE`
* Register streams previously generated and saved by the Uzebox

When a loose SID file is selected for the first time, UzeSID runs its 6502 player code and captures the resulting SID-register writes into SPI RAM. The resulting stream is then saved into the writable database inside `UZESID.UZE`.

Later playback can load the cached register stream directly without repeating the pre-emulation step.

The file browser merges loose SD-card SID files with songs embedded in the database. A database-only song therefore remains selectable even when its original `.SID` file is not present on the card.

An SD card and Uzenet-compatible SPI expansion RAM are required.

## Features

* Plays supported PSID v1 and PSID v2 files
* Supports multiple subtunes
* Supports PAL and NTSC clock rates
* Supports VBI-timed and CIA-timed songs
* Detects high-frequency CIA playback rates
* Preserves ordered SID writes, including repeated gate and test-bit transitions
* Pre-emulates SID code into compact register streams
* Supports persistent cached-stream playback
* Includes a writable LIDX/USDC database appended to `UZESID.UZE`
* Merges loose SD-card files and database-only songs into one browser
* Suppresses duplicate browser entries when the same SID exists in both places
* Supports previous, restart, pause, play, fast-forward, next, and subtune controls
* Supports SNES controllers and Super Mouse cursor movement
* Automatically detects installed SPI RAM capacity
* Supports 128 KiB and larger SPI RAM configurations
* Uses native 15.72 kHz SID synthesis by default
* Provides an optional 7.86 kHz interpolated build
* Includes a desktop host converter for preparing embedded songs
* Supports persistent runtime database updates without resizing the ROM file

RSID operating-system behavior is not implemented. RSID files generally expect a more complete Commodore 64 environment than UzeSID provides.

## Emulation

* [CUzeBox](https://github.com/Jubatian/cuzebox) supports Uzebox SPI expansion RAM.
* [CUzeBoxESP8266](https://github.com/weber21w/cuzebox-8266) supports larger SPI RAM configurations.

The amount of emulated SPI RAM affects the largest SID-register stream that can be loaded.

## Requirements

* Uzebox with Uzenet-compatible SPI expansion RAM

  * 128 KiB is the minimum supported configuration
  * Larger modules are recommended for long or register-heavy songs
* SD card formatted as FAT16 or FAT32
* Uzebox bootloader or ISP programmer
* AVR-GCC toolchain
* Python 3
* Host C compiler for the desktop SID converter
* Sufficient SD-card space for the appended writable database

### SPI RAM capacity

The first 64 KiB SPI RAM bank is reserved for the emulated C64 memory space. An additional 2 KiB is reserved for browser and capture scratch data.

On 128 KiB hardware, approximately 63,488 bytes remain for one generated or cached register stream. This is sufficient to run UzeSID and play smaller streams, but some long or high-rate songs require more RAM.

The default database build targets 64 SPI RAM banks, or 4 MiB. To build an embedded database limited to 128 KiB hardware, use:

```bash
make clean-db
make DB_SPI_BANKS=2
```

The database builder will reject embedded streams that do not fit the selected target capacity.

## Project Layout

Place the `UzeSID` directory under `uzebox-master/demos/`:

```text
uzebox-master/
└── demos/
    └── UzeSID/
        ├── UzeSID.c
        ├── UzeSID.h
        ├── Cache.c
        ├── Cache.h
        ├── Emulation.c
        ├── Emulation.h
        ├── soundMixerCustom.s
        ├── gameinfo.properties
        ├── data/
        │   ├── tiles.inc
        │   ├── tiles.png
        │   └── tiles.xml
        ├── db-tool/
        │   ├── sids/
        │   ├── host-converter/
        │   │   ├── Makefile
        │   │   ├── uzesid_capture.c
        │   │   └── stubs/
        │   ├── build_custom_database.py
        │   ├── pack_uze_dependency.py
        │   ├── validate_database.py
        │   └── Songlengths.md5
        └── default/
            └── Makefile
```

## Building

Navigate to the build directory:

```bash
cd uzebox-master/demos/UzeSID/default
```

### Default native 15.72 kHz build

```bash
make
```

Plain `make` builds the release configuration:

* Native 15.72 kHz SID synthesis
* Persistent database reads and writes
* Raw-SID pre-emulation and capture
* Eight-line adaptive rendering window
* Normal output level

The native synthesizer calculates every DAC sample directly. It includes optimized pitch conversion, noise generation, oscillator synchronization, envelope processing, SPI read-ahead, and register-stream decoding.

The explicit native target is also available:

```bash
make native15
```

### Optional 7.86 kHz interpolated build

```bash
make interp8
```

This calculates the SID core at approximately 7.86 kHz and linearly interpolates the intermediate samples to the 15.72 kHz Uzebox output rate.

The interpolated build uses less CPU time but retains:

* The same database format
* The same cached streams
* Raw-SID capture
* Persistent database writing
* The same controls and browser behavior

Cached streams are interchangeable between native and interpolated builds.

### Build comparison

| Command         | SID synthesis | Interpolation | Intended use            |
| --------------- | ------------: | ------------: | ----------------------- |
| `make`          |     15.72 kHz |            No | Default release build   |
| `make native15` |     15.72 kHz |            No | Explicit native rebuild |
| `make interp8`  |      7.86 kHz |           Yes | Additional CPU margin   |

Build-configuration stamps prevent objects from one synthesis mode from being reused by another.

## Native Rendering Window

Native synthesis temporarily reduces active rendering while the next audio buffer is generated. The normal 24-line interface is restored before input and GUI processing, so controls and cursor movement still update every frame.

The default native rendering window is eight scanlines:

```bash
make
```

It can be adjusted at build time:

```bash
make UZESID_NATIVE_RENDER_LINES=12
```

Accepted values are 1 through 24:

* Lower values reserve more CPU time for audio synthesis.
* Higher values render more of the display during audio processing.
* A value of 24 disables the temporary rendering reduction.

The default value of 8 has been tested with demanding SID playback while maintaining full-speed native synthesis and smooth GUI updates.

See `NATIVE_15KHZ.md` for additional implementation details.

## Clean Builds

Remove generated AVR build files:

```bash
make clean
```

Then rebuild the default native version:

```bash
make
```

`make clean` does not delete the generated writable database.

To force the embedded database to be regenerated:

```bash
make clean-db
make
```

Useful maintenance targets include:

```bash
make host-converter
make validate-db
make custom-db-help
```

## Build Output

The build produces files including:

```text
UzeSID.elf
UzeSID.hex
UzeSID.eep
UzeSID.lss
UzeSID.map
UzeSID.uze
```

Use `UzeSID.hex` when flashing directly with an ISP programmer.

For normal use, copy the complete generated `UzeSID.uze` file to the SD card as:

```text
UZESID.UZE
```

UzeSID opens `UZESID.UZE` at runtime when reading or updating its appended database.

Do not copy only the raw program image. The complete `.uze` file contains both the executable program and the appended writable database.

## Embedded Writable Database

The build generates a fixed-size LIDX/USDC database and appends it after the packed 60 KiB program area.

The database contains:

* Song lengths indexed by complete-file MD5
* Embedded register streams
* Subtune metadata
* Directory entries
* Allocation bitmap
* Preallocated writable free space

The database begins at byte 61,952 in the generated `.uze` file. The packaging tool verifies the `LIDX` header at that location before the build succeeds.

A successful build prints a line similar to:

```text
Packed database: ... verified LIDX at offset 61952 ...
```

Packaging errors stop the build instead of silently producing a ROM without its embedded streams.

`UZESID.UZE` must remain writable on the SD card. Runtime cache updates modify preallocated sectors inside the file; the ROM is never resized.

Replacing `UZESID.UZE` also replaces any streams that the hardware previously learned and saved.

## Adding Embedded SID Files

Place SID files intended for distribution in:

```text
UzeSID/db-tool/sids/
```

The normal build automatically:

1. Scans `db-tool/sids/`.
2. Parses each PSID file.
3. Determines its subtune count and playback timing.
4. Looks up or assigns each subtune’s duration.
5. Runs the desktop host converter.
6. Captures ordered SID-register writes.
7. Compacts the captured stream.
8. Builds the LIDX/USDC database.
9. Regenerates `prebuilt_sids.inc`.
10. Appends and verifies the database in `UzeSID.uze`.

Embedded songs appear in the merged browser even when their original `.SID` files are absent from the SD-card root.

The generated database defaults to:

* 128 MiB writable tail
* 512 directory entries
* 64 SPI RAM banks as the playback target
* Up to eight parallel converter jobs, depending on the host CPU

Examples:

```bash
make clean-db
make DB_TAIL_SIZE=64M
```

```bash
make clean-db
make DB_TAIL_SIZE=256M DB_DIR_ENTRIES=1024 DB_JOBS=8
```

For details about song-length overrides, automatic sizing, manifests, target SPI capacity, and standalone database generation, see:

```text
db-tool/CUSTOM_DATABASES.md
```

## Song Lengths

The database builder uses MD5-based song-length data in this order:

1. `Songlengths.override.md5`
2. `Songlengths.md5`
3. `Songlengths.local.md5`
4. Configured default duration

The normal default duration is three minutes when no matching length is available.

`Songlengths.local.md5` is intended for songs absent from the main database. `Songlengths.override.md5` is intended for deliberate corrections to existing entries.

## Host Converter

The desktop converter is located at:

```text
UzeSID/db-tool/host-converter/
```

It compiles the same capture and emulation core used by the Uzebox build and converts PSID subtunes into ordered register streams on the development computer.

The following directory is required and must remain in the repository:

```text
UzeSID/db-tool/host-converter/stubs/
```

The stubs provide host-compatible replacements for AVR, Uzebox, Petit FatFs, and SPI RAM declarations.

Generated host executables and object files should not be committed, but the source files and `stubs/` directory are required for reproducible database builds.

## SD-Card Layout

A typical SD-card root is:

```text
/
├── UZESID.UZE
├── COMMANDO.SID
├── BARBARIN.SID
├── TURRICAN.SID
└── other SID files...
```

Only `UZESID.UZE` is required for songs already embedded in its database.

Additional loose `.SID` files must be placed in the root directory. Subdirectory scanning is not currently supported by the Uzebox browser.

## Controls

* **START:** Open the merged loose-file and embedded-database browser
* **SELECT:** Open the persistent cached-stream browser
* **D-pad:** Move the cursor
* **Super Mouse movement:** Move the cursor
* **Y:** Activate the control under the cursor
* **Super Mouse left button:** Activate the control under the cursor

The player bar provides controls for:

* Previous
* Restart
* Pause
* Play
* Fast-forward
* Next
* File browsers
* Volume
* Color mask
* Save preferences

## Supported SID Files

UzeSID is intended for PSID files that can run within its lightweight 6502 and SID pre-emulation environment.

Compatibility can depend on:

* The CPU instructions used by the player
* VBI or CIA timing
* PAL or NTSC clock expectations
* Unsupported illegal CPU instructions
* Dependencies on unimplemented C64 hardware
* The selected subtune duration
* Available SPI RAM
* Generated register-stream size
* The number of ordered SID writes produced in one replay tick

RSID operating-system behavior is not supported.

## Current Limits

* Loose `.SID` scanning is limited to the SD-card root.
* Browser names use fixed 32-byte records.
* The merged browser supports up to 63 entries.
* A 128 KiB SPI RAM module leaves approximately 63,488 bytes for one register stream.
* SID synthesis is optimized for the ATmega644 rather than cycle-exact.
* Some optional Uzebox kernel features are disabled to preserve flash, SRAM, and CPU time.
* Runtime database writes cannot extend the `.uze` file.
* The default generated database is large because writable capacity is preallocated.

## Notes

UzeSID operates close to the ATmega644 flash, SRAM, and CPU limits.

Native 15.72 kHz synthesis is now the recommended and default build. The optimized native path has been tested with register-heavy, pitch-heavy, and noise-heavy music while maintaining full-speed playback and smooth interface updates.

The 7.86 kHz interpolated build remains available when additional CPU margin is preferred.
