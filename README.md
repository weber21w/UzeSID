# UzeSID

Commodore 64 SID music player for Uzebox.

UzeSID plays supported `.sid` music files using a lightweight 6502 and SID emulation system designed for the ATmega644. Raw SID files are pre-emulated into compact SID-register streams, which can then be cached for faster subsequent playback.

The player supports both an interpolated 7.86 kHz synthesis mode and an experimental native 15.72 kHz mode.

## Overview

UzeSID is a standalone PSID music player for the Uzebox platform.

It can play music from:

* `.SID` files stored in the root directory of an SD card
* Pre-converted songs embedded in the `UZESID.UZE` database
* Register streams previously generated and saved by the Uzebox

When a raw SID file is selected for the first time, UzeSID pre-emulates the song and creates a compact register dump in expansion RAM. When database writing is enabled, the resulting stream is saved into the writable database appended to `UZESID.UZE`.

On later playback, the cached register stream can be loaded directly without repeating the full SID emulation step.

An SD card and Uzenet-compatible SPI expansion RAM are required.

## Features

* Plays supported PSID music files
* Supports multiple subtunes
* Supports PAL and NTSC clock rates
* Supports VBI-timed and CIA-timed songs
* Detects high-frequency CIA playback rates
* Pre-emulates SID code into compact register streams
* Supports cached register-stream playback
* Includes a writable database appended to `UZESID.UZE`
* Merges SD-card SID files and embedded database songs into one browser
* Supports previous, next, pause, fast-forward, and subtune controls
* Automatically detects the installed SPI RAM capacity
* Supports 128 KiB through multi-megabyte SPI RAM configurations
* Provides standard and native 15.72 kHz synthesis modes
* Provides optional reduced-volume builds for additional audio headroom
* Includes a desktop host converter for preparing embedded songs

RSID playback is not currently advertised or guaranteed. RSID files generally expect a more complete Commodore 64 runtime environment than the lightweight pre-emulation system used by UzeSID.

## Emulation

* [CUzeBox](https://github.com/Jubatian/cuzebox) supports Uzebox SPI expansion RAM.
* [CUzeBoxESP8266](https://github.com/weber21w/cuzebox-8266) supports larger SPI RAM configurations.

The available SPI RAM capacity affects the maximum size of raw SID files and generated register streams that can be loaded.

## Requirements

* Uzebox with Uzenet-compatible SPI expansion RAM

  * 128 KiB minimum
  * Larger RAM modules are recommended for long or complex songs
* SD card formatted as FAT16 or FAT32
* Uzebox bootloader or ISP programmer
* AVR-GCC toolchain
* Python 3 for database-generation tools
* A host C compiler for the desktop SID converter

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
        ├── db-tool/
        ├── host-converter/
        └── default/
            └── Makefile
```

## Building

Navigate to the build directory:

```bash
cd uzebox-master/demos/UzeSID/default
```

The build variants all generate the same output filenames. Copy or rename `UzeSID.uze` after each build when retaining more than one version.

### Standard build

```bash
make
```

The standard build generates SID samples internally at approximately 7.86 kHz and interpolates them to the approximately 15.72 kHz Uzebox audio output rate.

This mode uses less CPU time than native synthesis and is the recommended compatibility build.

### Quiet standard build

```bash
make quiet
```

This uses the standard 7.86 kHz synthesis and interpolation path, but shifts each SID voice sample right by one bit before mixing.

The result is approximately 6 dB quieter and provides additional output headroom.

### Native 15.72 kHz build

```bash
make native15
```

This calculates every output sample directly at approximately 15.72 kHz instead of generating alternating samples through interpolation.

Native synthesis may improve:

* High-frequency waveform detail
* Noise reproduction
* Oscillator synchronization
* Ring modulation
* Fast SID register effects

It also consumes substantially more CPU time than the standard build.

### Quiet native 15.72 kHz build

```bash
make native15-quiet
```

This combines native 15.72 kHz synthesis with the one-bit per-voice attenuation used by the quiet standard build.

Use this target to test native synthesis with approximately 6 dB of additional mixer headroom.

### Build comparison

| Command               | SID synthesis | Interpolation | Approximate level |
| --------------------- | ------------: | ------------: | ----------------: |
| `make`                |      7.86 kHz |           Yes |            Normal |
| `make quiet`          |      7.86 kHz |           Yes |             −6 dB |
| `make native15`       |     15.72 kHz |            No |            Normal |
| `make native15-quiet` |     15.72 kHz |            No |             −6 dB |

Each target performs the appropriate configuration rebuild.

A manual clean build can also be performed with:

```bash
make clean
make
```

To rebuild the embedded SID database from scratch:

```bash
make clean-db
make
```

A successful database-enabled build should report that the appended database was verified at its configured ROM offset.

## Build Output

The build produces:

```text
UzeSID.hex
UzeSID.uze
UzeSID.elf
UzeSID.map
```

Use `UzeSID.hex` when flashing directly with an ISP programmer.

For normal use, copy `UzeSID.uze` to the SD card and launch it through the Uzebox bootloader.

The writable database is appended directly to `UzeSID.uze`, so the complete generated file must be copied to the SD card. Do not replace it with only the raw program image.

## Adding Embedded SID Files

Place SID files intended for pre-conversion in the configured source directory under:

```text
UzeSID/db-tool/
```

The database builder uses the host converter to:

1. Parse each SID file.
2. Detect its playback timing.
3. Pre-emulate each supported subtune.
4. Generate compact SID-register streams.
5. Build the writable database image.
6. Append the database to `UzeSID.uze`.

Embedded songs appear in the same browser as `.SID` files located on the SD-card root.

A song does not need to remain as a separate `.SID` file on the card when all required subtunes are present in the embedded database.

## Host Converter

The desktop converter is located at:

```text
UzeSID/host-converter/
```

It compiles the SID pre-emulation system for the host computer and is used while building the embedded database.

The following directory must remain in the repository:

```text
UzeSID/host-converter/stubs/
```

The stubs provide desktop-compatible replacements for AVR, Uzebox, Petit FatFs, and SPI RAM headers. This allows the converter to compile the same `UzeSID.c`, `Emulation.c`, and `Cache.c` source modules with a normal host compiler.

Generated host executables and object files should not be committed, but the `stubs/` source directory is required.

## SD-Card Layout

A typical SD-card layout is:

```text
/
├── UZESID.UZE
├── COMMANDO.SID
├── BARBARIN.SID
├── TURRICAN.SID
└── other SID files...
```

Only `UZESID.UZE` is required for songs already embedded in its database.

Raw `.SID` files should be placed in the root directory of the card.

## Supported SID Files

UzeSID is intended for PSID files that can run within its lightweight 6502 and SID pre-emulation environment.

Compatibility can depend on:

* The 6502 instructions used by the player
* Whether the song uses VBI or CIA timing
* The expected PAL or NTSC clock
* Unsupported hardware dependencies
* Unsupported illegal CPU instructions
* The amount of available SPI RAM
* The size and duration of the generated register stream

Songs included in the embedded database are pre-converted during the build and do not require the original `.SID` file to remain on the SD card.

## Notes

UzeSID operates near the ATmega644 flash, SRAM, and CPU limits. Some optional Uzebox kernel features are disabled to preserve enough resources for SID playback, filesystem access, expansion RAM, caching, and the user interface.

The native 15.72 kHz build is experimental. The standard interpolated build remains the recommended compatibility option.
