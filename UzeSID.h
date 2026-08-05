#ifndef UZESID_H
#define UZESID_H

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <uzebox.h>
#include <spiram.h>
#include <petitfatfs/pffconf.h>
#include <petitfatfs/diskio.h>
#include <petitfatfs/pff.h>
#include <stdint.h>

#ifndef UZESID_NO_TILE_DATA
#include "data/tiles.inc"
#endif
#include "Cache.h"

#ifndef UZESID_BASE_TYPES_DEFINED
#define UZESID_BASE_TYPES_DEFINED 1
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif
#ifndef UZESID_SIGNED_BASE_TYPES_DEFINED
#define UZESID_SIGNED_BASE_TYPES_DEFINED 1
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
#endif
#ifndef UZESID_64_TYPES_DEFINED
#define UZESID_64_TYPES_DEFINED 1
typedef uint64_t u64;
typedef int64_t  s64;
#endif
#ifndef CYCLE_T_DEFINED
typedef u32 cycle_t;
#define CYCLE_T_DEFINED 1
#endif

extern u8 vram[];
extern u8 mix_buf[];
extern volatile u8 mix_bank;
extern u16 joypad1_status_lo, joypad2_status_lo;
extern u16 joypad1_status_hi, joypad2_status_hi;
extern BYTE rcv_spi(void);

#define SD_SELECT()      PORTD &= ~(1 << 6)
#define SD_DESELECT()    PORTD |=  (1 << 6)
#ifdef __AVR__
#define Wait200ns() do { asm volatile("lpm\n\tlpm\n\t"); } while(0)
#else
#define Wait200ns() do { } while(0)
#endif

#define UZENET_EEPROM_ID0 32
#define UZENET_EEPROM_ID1 (UZENET_EEPROM_ID0 + 1)

#define UZE_SAMPLES_PER_FRAME 262
#define UZE_VIDEO_HZ          60
/* Uzebox video is always 60 Hz.  The SID clock remains a property of the
 * source tune (PAL or NTSC) and is selected from the PSID/UZSD metadata. */
#ifndef SID_DEFAULT_CLOCK_HZ
#define SID_DEFAULT_CLOCK_HZ   985248UL
#endif
/* The DAC always outputs 262 samples per 60 Hz frame (15.72 kHz).  The
 * release core synthesizes every DAC sample natively.  Set UZESID_SYNTH_DUP=2
 * for the optional 7.86 kHz core with stateful 2x linear interpolation. */
#ifndef UZESID_SYNTH_DUP
#define UZESID_SYNTH_DUP       2u
#endif
#if (UZESID_SYNTH_DUP != 1u) && (UZESID_SYNTH_DUP != 2u)
#error UZESID_SYNTH_DUP must be 1 or 2
#endif
#define UZESID_SYNTH_SPF       (UZE_SAMPLES_PER_FRAME / UZESID_SYNTH_DUP)
#ifndef UZESID_NATIVE_RENDER_LINES
#define UZESID_NATIVE_RENDER_LINES 8u
#endif
#if (UZESID_NATIVE_RENDER_LINES < 1u) || (UZESID_NATIVE_RENDER_LINES > 24u)
#error UZESID_NATIVE_RENDER_LINES must be between 1 and 24
#endif
#define EMU_RATE_HZ            (UZESID_SYNTH_SPF * UZE_VIDEO_HZ)
/* ADSR changes far more slowly than oscillator phase.  Updating it at about
 * 491 Hz removes almost all envelope-state work while oscillator phase,
 * waveform generation, sync, ring modulation, and noise remain at the current
 * synthesis rate. Gate transitions force an immediate update. */
#ifndef UZESID_ENV_DECIMATE
#define UZESID_ENV_DECIMATE    (32u / UZESID_SYNTH_DUP)
#endif

#define SPI_BANK_SIZE (64UL*1024UL)
#define UZESID_SPI_CPU_BANK      0u
#define UZESID_SPI_CPU_BASE      0UL
#define UZESID_SPI_TEMP_BASE     (64UL * 1024UL)
#define UZESID_SPI_SCRATCH_SIZE  0x0800UL
#define UZESID_TEMP_SPI_OFS      (UZESID_SPI_TEMP_BASE + UZESID_SPI_SCRATCH_SIZE)
#define UZESID_RAWLIST_BASE      UZESID_SPI_TEMP_BASE
#define UZESID_RAWLIST_MAX       63
#define UZESID_SELECTED_PATH_OFS  (UZESID_RAWLIST_BASE + ((u32)UZESID_RAWLIST_MAX * 32UL))
#define UZESID_RAW_REQUIRED_RAM  (UZESID_SPI_TEMP_BASE + SPI_BANK_SIZE)
/* The cached-song browser may reuse bank 0 after capture has finished: cached
 * playback lives in banks 1+, and a later raw SID load reinitializes C64 RAM. */
#define UZESID_CACHE_BROWSER_BASE        0UL
#define UZESID_CACHE_BROWSER_RECORD_SIZE 36u

#ifndef UZESID_ENABLE_CACHE_WRITE
#define UZESID_ENABLE_CACHE_WRITE 0
#endif

static inline void ofs_to_bank_addr(u32 ofs, u8 *bank, u16 *addr){
    *bank = (u8)(ofs >> 16);
    *addr = (u16)(ofs & 0xffffu);
}

typedef struct osid_t osid_t;

void osid_reset(osid_t *sid);
u32  osid_read (osid_t *sid, u32 adr, cycle_t now);
void osid_write(osid_t *sid, u32 adr, u32 byte, cycle_t now, u8 rmw);

extern u32 f_rand_seed;
inline static u8 f_rand(void){
    f_rand_seed = f_rand_seed * 1103515245u + 12345u;
    return (u8)(f_rand_seed >> 16);
}

void SIDInit(void);
void SIDExit(void);
void SIDReset(cycle_t now);
void SIDCalcBuffer(u8 *buf, int count);
void SIDExecute(void);
void SIDSetReplayFreq(int freq);
void SIDSetClockHz(u32 clock_hz);
void SIDAdjustSpeed(int percent);
void SIDWriteRegister(u8 reg, u8 val);
void SIDBeginRegisterBatch(void);
void SIDEndRegisterBatch(void);
void UzeSID_CopyRegs(u8 *dst, u8 count);

void cia_tl_write(u8 byte);
void cia_th_write(u8 byte);
u32  sid_read (u32 adr, cycle_t now);
void sid_write(u32 adr, u32 byte, cycle_t now, u8 rmw);

u64 GetTicks_usec(void);
void Delay_usec(u32 usec);

#define UZESID_FRAME_SAMPLES 262
#define SAMPLE_RATE          (UZESID_FRAME_SAMPLES * 60UL)
#define BUFFER_SIZE          UZESID_FRAME_SAMPLES

#define CONT_BTN_W 16
#define CONT_BAR_X 40
#define CONT_BAR_W (13 * CONT_BTN_W)
#define CONT_BTN_H CONT_BTN_W
#define CONT_BAR_Y 0
#define CONT_BAR_H CONT_BTN_H

/* Keep the complete UzeMOD-compatible font sheet.  Tiles 64..127 are the
 * inverse font used by the file-browser selection bar. */
#define TILE_CURSOR 129
#define TILE_WIN_TLC (TILE_CURSOR + 1)
#define TILE_WIN_TRC (TILE_WIN_TLC + 1)
#define TILE_WIN_BLC (TILE_WIN_TRC + 3)
#define TILE_WIN_BRC (TILE_WIN_BLC + 1)
#define TILE_WIN_TBAR (TILE_WIN_TRC + 1)
#define TILE_WIN_BBAR (TILE_WIN_TBAR + 1)
#define TILE_WIN_LBAR (TILE_WIN_BRC + 1)
#define TILE_WIN_RBAR (TILE_WIN_LBAR + 1)
#define TILE_WIN_SCRU (TILE_WIN_RBAR + 1)
#define TILE_WIN_SCRD (TILE_WIN_SCRU + 1)

#define PS_LOADED  1
#define PS_PLAYING 2
#define PS_STOP    4
#define PS_PAUSE   8
#define PS_SHUFFLE 16
#define PS_DRAWN   32

#define PTIME_X (SCREEN_TILES_H - 9)
#define PTIME_Y 2

#define DEFAULT_COLOR_MASK 0b00000111
/* Masks with DDRC bits 7, 5, and 2 all clear disconnect every useful color
 * output.  Test those three bits directly instead of carrying a 32-byte table. */
#define UZESID_COLOR_MASK_INVALID(mask) (((mask) & 0xA4u) == 0u)

extern FATFS fs;
extern u16 oldpad, pad;
extern u8 play_state;
extern u8 masterVolume;

#define UZESID_PLAYER_FRAME_NO_TICK 0
#define UZESID_PLAYER_FRAME_TICK 1
#define UZESID_PLAYER_FRAME_LOOPED 2
#define UZESID_PLAYER_FRAME_STOPPED 3
#define UZESID_PLAYER_FRAME_ERROR -1

typedef struct {
    void *user;
    void (*reset)(void *user);
    void (*begin_batch)(void *user);
    void (*write_reg)(void *user, u8 reg, u8 val);
    void (*end_batch)(void *user);
} UzesidSidSink;

typedef struct {
    UzesidUzsdStream stream;
    UzesidSidSink sink;
    u8 enabled;
    u8 auto_loop;
    u8 last_error;
    u16 tick_hz;
    u16 tick_phase;
#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
    u8 direct_sid_sink;
#endif
    u32 loop_count;
} UzesidPlayer;

int UzesidPlayerOpenStream(UzesidPlayer *pl, const UzesidUzsdStream *stream, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop);
int UzesidPlayerOpenSpiTemp(UzesidPlayer *pl, u32 base_offset, u32 total_size, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop);
int UzesidPlayerOpenCachedEntry(UzesidPlayer *pl, const UzesidUsdc *usdc, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop);
int UzesidPlayerRestart(UzesidPlayer *pl);
void UzesidPlayerStop(UzesidPlayer *pl);
int UzesidPlayerStepVideoFrame(UzesidPlayer *pl);

typedef struct { u8 reserved; } UzesidUzeSidSinkState;
void UzesidUzeSidSinkReset(void *user);
void UzesidUzeSidSinkBeginBatch(void *user);
void UzesidUzeSidSinkWriteReg(void *user, u8 reg, u8 val);
void UzesidUzeSidSinkEndBatch(void *user);
void UzesidUzeSidMakeSink(UzesidSidSink *sink, UzesidUzeSidSinkState *state);

#ifndef MONO_GAIN_V1_Q12_4
#define MONO_GAIN_V1_Q12_4 16
#endif
#ifndef MONO_GAIN_V2_Q12_4
#define MONO_GAIN_V2_Q12_4 16
#endif
#ifndef MONO_GAIN_V3_Q12_4
#define MONO_GAIN_V3_Q12_4 16
#endif
#ifndef MONO_GAIN_V4_Q12_4
#define MONO_GAIN_V4_Q12_4 16
#endif
static const u16 k_gain_q12_4[3] = { MONO_GAIN_V1_Q12_4, MONO_GAIN_V2_Q12_4, MONO_GAIN_V3_Q12_4 };
extern u8 k_gain_q12_4_mv[3];

#define SID_6581 0
#define SID_8580 1
#ifndef SID_MODEL
#define SID_MODEL SID_6581
#endif

#include "tables.inc"

#endif
