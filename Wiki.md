{{Gameinfo
|title          = UzeSID
|image          = Image:UzeSIDScreen.png
|caption        = Commodore 64 SID music player for Uzebox
|genres         =
|genre          = Music Player / Utility
|developers     =
|developer      = Lee Weber (D3thAdd3r)
|code licenses  =
|code license   = [[Image:Gplv3.png|GNU General Public License version 3]] <span style="color:red"><br/>or (at your option) any later version.</span>
|media licenses =
|media license  = [[Image:CC_by_sa.png|Creative Commons Attribution Share-Alike]] version 3.0
|engines        =
|engine         = Uzebox 3.0
|video modes    =
|video mode     = 3
|latest release = v1.0
|release date   = 8/5/26
|languages      =
|language       = English
}}

'''UzeSID''' is a Commodore 64 SID music player for the Uzebox.

It plays supported PSID music using a lightweight 6502 and SID emulation system designed for the ATmega644. SID files are pre-emulated into compact streams of ordered SID-register writes, which are then played using an optimized SID synthesizer.

Native 15.72 kHz SID synthesis is used by default. An optional 7.86 kHz synthesis mode with interpolation is also available.

UzeSID supports loose SID files stored on an SD card as well as pre-converted songs embedded in a writable database appended to the UzeSID ROM.

==Video==

[https://www.youtube.com/watch?v=rDpJzCZ6fUg Watch the UzeSID demonstration on YouTube]

==Current Features==

* Plays supported PSID music files
* Native 15.72 kHz SID synthesis
* Optional 7.86 kHz synthesis with interpolation
* Lightweight 6502 emulation
* Multiple subtune support
* PAL and NTSC clock support
* VBI-timed and CIA-timed song support
* Detection of high-frequency CIA playback rates
* Ordered SID-register write capture
* Compact cached register-stream playback
* Writable database appended to UZESID.UZE
* Embedded database songs and loose SD-card files shown in one browser
* Duplicate browser entries automatically suppressed
* Previous, restart, pause, play, fast-forward, and next controls
* Subtune selection
* SNES controller support
* Super Mouse cursor support
* Automatic SPI RAM capacity detection
* Support for 128 KiB and larger SPI RAM configurations
* Desktop SID converter for preparing embedded songs
* Runtime caching of newly converted SID files
* Persistent database updates without resizing the ROM file

==SID Compatibility==

UzeSID is intended for PSID files that can run within its lightweight Commodore 64 emulation environment.

Compatibility can depend on:

* CPU instructions used by the SID player
* VBI or CIA playback timing
* PAL or NTSC clock expectations
* Unsupported illegal CPU instructions
* Dependencies on unimplemented Commodore 64 hardware
* Available SPI RAM
* Song duration
* Generated register-stream size
* Number of SID-register writes generated during each replay tick

RSID operating-system behavior is not currently supported.

==Audio Modes==

===Native 15.72 kHz===

Native 15.72 kHz synthesis is the default and recommended configuration.

Every Uzebox audio sample is generated directly by the SID synthesizer. The native audio path includes optimized:

* Pitch conversion
* Noise generation
* Oscillator synchronization
* Ring modulation
* Envelope processing
* Register-stream decoding
* SPI RAM read-ahead

The native mode has been tested with register-heavy, pitch-heavy, and noise-heavy music while maintaining full-speed playback and smooth interface updates.

===Interpolated 7.86 kHz===

The optional interpolated build calculates the SID core at approximately 7.86 kHz and interpolates intermediate samples to the approximately 15.72 kHz Uzebox output rate.

This mode provides additional CPU margin while using the same database and cached-register-stream formats.

==Controls==

* '''D-pad''' — Move the cursor
* '''Super Mouse movement''' — Move the cursor
* '''Y''' — Activate the selected control
* '''Super Mouse left button''' — Activate the selected control
* '''START''' — Open the merged SID and embedded-song browser
* '''SELECT''' — Open the cached-stream browser

The player interface includes controls for:

* Previous song
* Restart song
* Pause
* Play
* Fast-forward
* Next song
* File browser
* Cached-stream browser
* Volume
* Display color mask
* Save preferences

==Hardware Requirements==

* Uzebox with an ATmega644
* Uzenet-compatible SPI expansion RAM
** 128 KiB minimum
** Larger SPI RAM configurations are recommended
* FAT16 or FAT32 SD card
* Uzebox bootloader or ISP programmer

The first 64 KiB SPI RAM bank is reserved for the emulated Commodore 64 address space. Additional space is used for browser data, capture buffers, and the active SID-register stream.

A 128 KiB expansion provides approximately 63 KiB for the active stream. Long songs and songs with high SID-register activity may require more RAM.

==SD Card Layout==

A typical SD-card root directory is:

<pre>
/
├── UZESID.UZE
├── COMMANDO.SID
├── BARBARIN.SID
├── TURRICAN.SID
└── other SID files...
</pre>

Only UZESID.UZE is required for songs already embedded in its database.

Loose SID files must currently be placed in the root directory of the SD card.

==Writable Database==

UzeSID contains a fixed-size writable LIDX/USDC database appended to UZESID.UZE.

The database can contain:

* Pre-converted SID-register streams
* Runtime-generated register streams
* Subtune metadata
* Song-duration information
* Directory entries
* Preallocated writable space

When a loose SID file is selected for the first time, UzeSID:

# Loads the SID into SPI RAM.
# Runs its initialization and playback routines.
# Captures the resulting SID-register writes.
# Produces a compact register stream.
# Saves the stream into the writable database.

Later playback loads the cached stream directly without repeating the full pre-emulation process.

UZESID.UZE must remain writable on the SD card. Runtime database updates modify preallocated sectors inside the file and do not resize it.

Replacing UZESID.UZE also replaces any streams previously generated and saved by the hardware.

==Building==

Place the project under:

<pre>
uzebox-master/demos/UzeSID/
</pre>

Navigate to:

<pre>
cd uzebox-master/demos/UzeSID/default
</pre>

===Default native build===

<pre>
make
</pre>

This builds the default native 15.72 kHz version.

The explicit native target is also available:

<pre>
make native15
</pre>

===Interpolated build===

<pre>
make interp8
</pre>

This builds the optional 7.86 kHz SID synthesis mode with interpolation.

===Clean build===

<pre>
make clean
make
</pre>

===Rebuild the embedded database===

<pre>
make clean-db
make
</pre>

The generated UzeSID.uze contains both the program and the appended writable database. The complete file must be copied to the SD card as UZESID.UZE.

==Adding Embedded Songs==

Place SID files intended for inclusion in the built-in database under:

<pre>
UzeSID/db-tool/sids/
</pre>

The database builder:

# Parses each PSID file.
# Determines its subtune count.
# Detects playback timing.
# Looks up or assigns a duration for each subtune.
# Runs the desktop host converter.
# Captures ordered SID-register writes.
# Compresses each generated stream.
# Builds the writable database.
# Appends the database to UzeSID.uze.
# Verifies the database header and ROM offset.

Embedded songs appear in the same browser as loose SID files stored on the SD card.

==Emulation==

UzeSID can be tested in emulators that support Uzebox SPI expansion RAM.

* [https://github.com/Jubatian/cuzebox CUzeBox]
* [https://github.com/weber21w/cuzebox-8266 CUzeBoxESP8266]

The configured emulated SPI RAM capacity affects the largest SID-register stream that can be loaded.

==ROM==

UZESID.UZE[https://uzebox.org/forums/download/file.php?id=5415]

==Source==

[https://github.com/weber21w/UzeSID UzeSID source repository]

==Development Notes==

UzeSID operates close to the ATmega644 flash, SRAM, and CPU limits.

Several Uzebox kernel options are disabled to preserve enough resources for:

* Native SID synthesis
* 6502 pre-emulation
* Filesystem access
* SPI expansion RAM
* Persistent caching
* Database browsing
* User-interface rendering

Native synthesis temporarily reduces active rendering while the next audio buffer is generated. The normal interface is restored before input and GUI processing, allowing the controls and cursor to update every frame.

The default adaptive rendering window has been tested with demanding SID music without visible flicker or playback slowdown.

[[Category:Applications]]
[[Category:Music]]
[[Category:Uzebox software]]
[[Category:Games licensed under the GNU GPL v3]]
[[Category:Media licensed under an CC Attribution-ShareAlike license v3]]
[[Category:Free software]]
[[Category:Free culture]]