#ifndef UZESID_EMULATION_H
#define UZESID_EMULATION_H

#include <stdint.h>
#include "Cache.h"

#ifdef __cplusplus
extern "C" {
#endif

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
#endif
#ifndef CYCLE_T_DEFINED
#define CYCLE_T_DEFINED 1
typedef u32 cycle_t;
#endif

#ifndef SID_DEFAULT_CLOCK_HZ
#define SID_DEFAULT_CLOCK_HZ 985248UL
#endif

#define RAM_SIZE ((u32)0x10000UL)

enum
{
	UZESID_PSID_ERROR_NONE = 0,
	UZESID_PSID_ERROR_INVALID = 1,
	UZESID_PSID_ERROR_TRUNCATED = 2,
	UZESID_PSID_ERROR_TOO_LARGE = 3
};

void CPUInit(void);
void CPUExit(void);
void CPUExecute(u16 startadr, u8 init_ra, u8 init_rx, u8 init_ry, cycle_t max_cycles);

void MemoryInit(void);
void MemoryExit(void);
void MemoryClear(void);

u8 CPUPeekByte(u16 adr);
void CPUPokeByte(u16 adr, u8 byte);
void CPUPokeBlock(u16 adr, const u8 *src, u16 len);

void cia_tl_write(u8 byte);
void cia_th_write(u8 byte);
u32 sid_read(u32 adr, cycle_t now);
void sid_write(u32 adr, u32 byte, cycle_t now, u8 rmw);
void SIDReset(cycle_t now);
void UzeSID_CopyRegs(u8 *dst, u8 count);


extern int number_of_songs;
extern int current_song;
extern u16 play_adr;
extern cycle_t cycles_per_second;
extern u16 cia_timer;

int UzesidParseMd5Hex(const char *hex, u8 md5[16]);

extern u8 g_uzesid_capture_enabled;
/* Capture preserves ordered SID register transitions within each player tick.
 * Repeated control-register writes (for example gate off/on) are musically
 * significant and must not be collapsed to one final value. */
void UzesidCaptureNoteWrite(u8 reg, u8 val);
extern u8 g_uzesid_workbuf[512];
extern u8 g_uzesid_psid_loaded;
extern u8 g_uzesid_psid_error;
extern u16 g_uzesid_init_adr;
extern u8 g_uzesid_play_adr_from_irq_vec;
extern u32 g_uzesid_speed_flags;

void EmulationUpdatePlayAdr(void);
u8 IsPSIDLoaded(void);
void SelectSong(u8 num);
#define UZESID_PSID_SCRATCH_SIZE 0x7c
u8 LoadPSIDFilePff(const char *file, u8 md5_out[16], UzesidUsdcEntry *scratch_entry, s16 requested_song);
u8 UzesidCaptureCurrentSongToSpi(u32 temp_ofs, u32 temp_capacity, u32 song_length_ms, u32 *out_total_size, u32 *out_song_length_ms, u16 *out_tick_hz);

#ifdef __cplusplus
}
#endif

#endif
