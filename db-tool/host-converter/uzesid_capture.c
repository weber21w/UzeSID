/*
 * Native UzeSID PSID -> UZSD converter.
 *
 * This intentionally links the same Emulation.c, capture encoder, SID write
 * behavior, and memory model as the AVR player.  Python is used only to
 * orchestrate files and pack the database; the expensive 6510 execution is C.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uzebox.h>
#include <petitfatfs/pff.h>
#include "Emulation.h"

#define HOST_SPI_MAX_BANKS 255u
#define HOST_SPI_BANK_SIZE 65536u
#define HOST_SPI_SIZE (HOST_SPI_MAX_BANKS * HOST_SPI_BANK_SIZE)
#define HOST_STREAM_OFFSET 0x10800u

static uint8_t g_host_spi_banks = HOST_SPI_MAX_BANKS;
static uint8_t g_target_spi_banks = 64u;
static uint8_t g_spiram[HOST_SPI_SIZE];
static FILE *g_pf_file;
static uint32_t g_seq_offset;

extern FATFS fs;
extern void SIDInit(void);

/* AVR / Uzebox globals referenced by linked code. */
uint8_t PORTB, PORTC, DDRB, DDRD, PINB, PINC, PIND;
volatile uint8_t PORTD, DDRC, OCR2A, SREG;
uint8_t SPDR, SPSR, SPCR;
volatile uint8_t JOYPAD_OUT_PORT, JOYPAD_IN_PORT;
uint8_t vram[32u * 32u];
uint8_t mix_buf[524];
volatile uint8_t mix_bank;
uint16_t joypad1_status_lo, joypad2_status_lo, joypad1_status_hi, joypad2_status_hi;
Sprite sprites[1];

static void die(const char *message)
{
    fprintf(stderr, "uzesid_capture: %s\n", message);
    exit(2);
}

static uint32_t spi_abs(uint8_t bank, uint16_t address)
{
    return ((uint32_t)bank << 16) | (uint32_t)address;
}

uint8_t SpiRamInitGetSize(void)
{
    return g_host_spi_banks;
}

void SpiRamReadInto(uint8_t bank, uint16_t address, void *dst, uint16_t length)
{
    uint32_t offset = spi_abs(bank, address);
    if(offset + length > (uint32_t)g_host_spi_banks * HOST_SPI_BANK_SIZE)
        die("SPI RAM read out of bounds");
    memcpy(dst, g_spiram + offset, length);
}

void SpiRamWriteFrom(uint8_t bank, uint16_t address, const void *src, uint16_t length)
{
    uint32_t offset = spi_abs(bank, address);
    if(offset + length > (uint32_t)g_host_spi_banks * HOST_SPI_BANK_SIZE)
        die("SPI RAM write out of bounds");
    memcpy(g_spiram + offset, src, length);
}

uint8_t SpiRamRead(uint8_t bank, uint16_t address)
{
    return g_spiram[spi_abs(bank, address)];
}

void SpiRamWrite(uint8_t bank, uint16_t address, uint8_t value)
{
    g_spiram[spi_abs(bank, address)] = value;
}

void SpiRamSeqWriteStart(uint8_t bank, uint16_t address)
{
    g_seq_offset = spi_abs(bank, address);
}

void SpiRamSeqWriteU8(uint8_t value)
{
    if(g_seq_offset >= (uint32_t)g_host_spi_banks * HOST_SPI_BANK_SIZE)
        die("sequential SPI RAM write out of bounds");
    g_spiram[g_seq_offset++] = value;
}

void SpiRamSeqWriteEnd(void)
{
}

void SpiRamSeqReadStart(uint8_t bank, uint16_t address)
{
    g_seq_offset = spi_abs(bank, address);
}

uint8_t SpiRamSeqReadU8(void)
{
    if(g_seq_offset >= (uint32_t)g_host_spi_banks * HOST_SPI_BANK_SIZE)
        die("sequential SPI RAM read out of bounds");
    return g_spiram[g_seq_offset++];
}

void SpiRamSeqReadEnd(void)
{
}

/* Petit FatFs host adapter: one active file, matching the AVR API. */
BYTE rcv_spi(void)
{
    return 0xffu;
}

FRESULT pf_mount(FATFS *filesystem)
{
    (void)filesystem;
    return FR_OK;
}

FRESULT pf_open(const char *path)
{
    long size;
    if(g_pf_file != NULL){
        fclose(g_pf_file);
        g_pf_file = NULL;
    }
    g_pf_file = fopen(path, "rb");
    if(g_pf_file == NULL)
        return 1;
    if(fseek(g_pf_file, 0, SEEK_END) != 0)
        return 1;
    size = ftell(g_pf_file);
    if(size < 0 || (unsigned long)size > UINT32_MAX)
        return 1;
    fs.fsize = (uint32_t)size;
    if(fseek(g_pf_file, 0, SEEK_SET) != 0)
        return 1;
    return FR_OK;
}

FRESULT pf_read(void *dst, UINT length, UINT *bytes_read)
{
    size_t count;
    if(g_pf_file == NULL)
        return 1;
    count = fread(dst, 1, length, g_pf_file);
    *bytes_read = (UINT)count;
    return ferror(g_pf_file) ? 1 : FR_OK;
}

FRESULT pf_write(const void *src, UINT length, UINT *bytes_written)
{
    (void)src;
    (void)length;
    *bytes_written = 0;
    return 1;
}

FRESULT pf_lseek(DWORD position)
{
    if(g_pf_file == NULL || fseek(g_pf_file, (long)position, SEEK_SET) != 0)
        return 1;
    return FR_OK;
}

FRESULT pf_opendir(DIR *directory, const char *path)
{
    (void)directory;
    (void)path;
    return 1;
}

FRESULT pf_readdir(DIR *directory, FILINFO *info)
{
    (void)directory;
    (void)info;
    return 1;
}

/* UI/kernel functions retained only to satisfy references in discarded code. */
void SetFontTilesIndex(uint8_t value) { (void)value; }
void SetTileTable(const char *value) { (void)value; }
void SetSpritesTileTable(const char *value) { (void)value; }
void ClearVram(void) {}
void WaitVsync(uint8_t frames) { (void)frames; }
void SetRenderingParameters(uint8_t first, uint8_t lines) { (void)first; (void)lines; }
void SetTile(uint8_t x, uint8_t y, uint8_t tile) { (void)x; (void)y; (void)tile; }
void DrawMap(uint8_t x, uint8_t y, const char *map) { (void)x; (void)y; (void)map; }
void SetMasterVolume(uint8_t value) { (void)value; }
uint16_t ReadJoypad(uint8_t port) { (void)port; return 0; }
void SoftReset(void) {}
void SetUserPostVsyncCallback(void (*callback)(void)) { (void)callback; }
uint8_t EepromReadByte(uint16_t address) { (void)address; return 0; }
void EepromWriteByte(uint16_t address, uint8_t value) { (void)address; (void)value; }
uint8_t EepromReadBlock(uint8_t id, void *block) { (void)id; (void)block; return 0; }
void EepromWriteBlock(const void *block) { (void)block; }
void PrintByte(uint8_t x, uint8_t y, uint8_t value, uint8_t flags) { (void)x; (void)y; (void)value; (void)flags; }
void PrintLong(uint8_t x, uint8_t y, uint32_t value) { (void)x; (void)y; (void)value; }
void PrintInt(uint8_t x, uint8_t y, int value, uint8_t flags) { (void)x; (void)y; (void)value; (void)flags; }
void SetSpriteVisibility(uint8_t sprite, uint8_t visible) { (void)sprite; (void)visible; }
uint8_t GetVsyncFlag(void) { return 1; }

static uint32_t parse_u32(const char *text, const char *name)
{
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 0);
    if(errno != 0 || end == text || *end != '\0' || value > UINT32_MAX){
        fprintf(stderr, "uzesid_capture: invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint32_t)value;
}

static void write_file(const char *path, const void *data, size_t length)
{
    FILE *output = fopen(path, "wb");
    if(output == NULL){
        fprintf(stderr, "uzesid_capture: cannot create %s: %s\n", path, strerror(errno));
        exit(2);
    }
    if(fwrite(data, 1, length, output) != length){
        fprintf(stderr, "uzesid_capture: write failed for %s: %s\n", path, strerror(errno));
        fclose(output);
        exit(2);
    }
    if(fclose(output) != 0){
        fprintf(stderr, "uzesid_capture: close failed for %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s INPUT.sid SUBTUNE_INDEX LENGTH_MS OUTPUT.uzsd [SPI_BANKS]\n"
        "\n"
        "SUBTUNE_INDEX is zero-based. LENGTH_MS controls how long the 6510\n"
        "play routine is pre-emulated. SPI_BANKS defaults to 64 and must be\n"
        "between 2 and 64; each bank is 64 KiB.\n",
        program);
}

int main(int argc, char **argv)
{
    const char *sid_path;
    const char *output_path;
    uint32_t requested_ms;
    uint32_t total_size = 0;
    uint32_t actual_ms = 0;
    uint16_t tick_hz = 0;
    uint32_t subtune_value;
    uint32_t spi_banks_value = 64u;
    uint32_t stream_capacity;
    UzesidUsdcEntry entry;

    if(argc != 5 && argc != 6){
        usage(argv[0]);
        return 2;
    }

    sid_path = argv[1];
    subtune_value = parse_u32(argv[2], "subtune index");
    requested_ms = parse_u32(argv[3], "length in milliseconds");
    output_path = argv[4];
    if(argc == 6)
        spi_banks_value = parse_u32(argv[5], "SPI bank count");
    if(spi_banks_value < 2u || spi_banks_value > 255u)
        die("target SPI bank count must be between 2 and 255");
    g_target_spi_banks = (uint8_t)spi_banks_value;
    g_host_spi_banks = HOST_SPI_MAX_BANKS;
    stream_capacity = (uint32_t)HOST_SPI_MAX_BANKS * HOST_SPI_BANK_SIZE - HOST_STREAM_OFFSET;
    if(subtune_value > 0x7fffu)
        die("subtune index is out of range");
    if(requested_ms == 0)
        die("length must be greater than zero");

    memset(&entry, 0, sizeof(entry));
    memset(g_spiram, 0xcc, sizeof(g_spiram));
    SIDInit();

    if(!LoadPSIDFilePff(sid_path, NULL, &entry, (s16)subtune_value)){
        fprintf(stderr, "uzesid_capture: SID load failed (error %u): %s\n",
            (unsigned)g_uzesid_psid_error, sid_path);
        return 1;
    }

    if(!UzesidCaptureCurrentSongToSpi(
            HOST_STREAM_OFFSET,
            stream_capacity,
            requested_ms,
            &total_size,
            &actual_ms,
            &tick_hz)){
        fprintf(stderr, "uzesid_capture: capture failed: %s subtune %lu\n",
            sid_path, (unsigned long)subtune_value);
        return 1;
    }

    if(total_size == 0 || total_size > stream_capacity)
        die("capture returned an invalid stream size");

    write_file(output_path, g_spiram + HOST_STREAM_OFFSET, total_size);
    fprintf(stdout,
        "{\"sid\":\"%s\",\"subtune\":%lu,\"requested_ms\":%lu,"
        "\"actual_ms\":%lu,\"tick_hz\":%u,\"bytes\":%lu,"
        "\"spi_banks\":%u,\"capture_banks\":%u,\"capacity\":%lu}\n",
        sid_path,
        (unsigned long)subtune_value,
        (unsigned long)requested_ms,
        (unsigned long)actual_ms,
        (unsigned)tick_hz,
        (unsigned long)total_size,
        (unsigned)g_target_spi_banks,
        (unsigned)g_host_spi_banks,
        (unsigned long)stream_capacity);
    return 0;
}
