#ifndef UZESID_CACHE_H
#define UZESID_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UZESID_BASE_TYPES_DEFINED
#define UZESID_BASE_TYPES_DEFINED 1
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define UZESID_LIDX_MAGIC 0x5844494cUL /* 'LIDX' */
#define UZESID_LIDX_REC_INLINE_SINGLE 0x0001U

#define UZESID_USDC_MAGIC 0x43445355UL /* 'USDC' */
#define UZESID_USDC_ENTRY_FREE      0
#define UZESID_USDC_ENTRY_LIVE      1
#define UZESID_USDC_ENTRY_BUILDING  2
#define UZESID_USDC_ENTRY_DELETED   3
#define UZESID_USDC_ENTRY_TYPE_USD  1

#define UZESID_UZSD_MAGIC 0x44535a55UL /* 'UZSD' */
#define UZESID_UZSD_VERSION_V3 3
#define UZESID_UZSD_VERSION_V4 4
#define UZESID_UZSD_VERSION UZESID_UZSD_VERSION_V3 /* runtime capture remains v3 */
#define UZESID_UZSD_INIT_REG_COUNT 25
#define UZESID_UZSD_HEADER_SIZE 49

/*
 * Playback and raw-SID capture are mutually exclusive on the AVR.  Keep their
 * transient byte buffers in one shared arena rather than reserving separate
 * storage for both.  160 bytes still amortizes several UZSD ticks per SPI-RAM
 * read while recovering enough SRAM to preserve the 512-byte stack margin.
 */
#ifndef UZESID_RUNTIME_SCRATCH_SIZE
#define UZESID_RUNTIME_SCRATCH_SIZE 160u
#endif
extern u8 g_uzesid_runtime_scratch[UZESID_RUNTIME_SCRATCH_SIZE];

#define UZESID_UZSD_OP_SKIP  0x00u
#define UZESID_UZSD_OP_PAIRS 0x01u
#define UZESID_UZSD_OP_MASK  0x02u
#define UZESID_UZSD_OP_END   0xffu

#define UZESID_UZSD_FLAG_TRUNCATED      0x01u
#define UZESID_UZSD_FLAG_EVENT_OVERFLOW 0x02u
#define UZESID_UZSD_FLAG_CAPACITY       0x04u
#define UZESID_UZSD_FLAG_MASK           0x07u
/* Byte 5 stores tick rate bits 0..7.  The previously reserved upper five
 * bits of byte 7 store bits 8..12, preserving the 49-byte stream header and
 * compatibility with existing 50/60 Hz streams. */
#define UZESID_UZSD_TICK_HI_SHIFT       3u
#define UZESID_UZSD_TICK_MAX            8191u

/* UZSD v4 compact event tokens. Registers 0..24 occupy the low five bits.
 * Codes 0..6 select a fixed signed delta; code 7 stores an absolute byte.
 * Low-five-bit values 25..31 are reserved for stream control tokens. */
#define UZESID_UZSD_V4_CTRL_END_TICK 25u
#define UZESID_UZSD_V4_CTRL_SKIP     26u
#define UZESID_UZSD_V4_CTRL_END      27u

typedef u8 (*UzesidReadAtFn)(void *user, u32 abs_offset, void *dst, u16 len);

typedef struct
{
	void *user;
	UzesidReadAtFn read_at;
} UzesidReader;

u16 UzesidRd16(const u8 *p);
u32 UzesidRd24(const u8 *p);
u32 UzesidRd32(const u8 *p);
int UzesidReadExact(const UzesidReader *io, u32 abs_offset, void *dst, u16 len);

#ifdef UZESID_HAVE_PFF
typedef struct
{
	u8 reserved;
} UzesidPffContext;

u8 UzesidPffReadAt(void *user, u32 abs_offset, void *dst, u16 len);
void UzesidPffInitReader(UzesidReader *io, UzesidPffContext *ctx);
#endif

typedef struct
{
	u8 reserved;
} UzesidSpiContext;

u8 UzesidSpiReadAt(void *user, u32 abs_offset, void *dst, u16 len);
void UzesidSpiInitReader(UzesidReader *io, UzesidSpiContext *ctx);

typedef struct
{
	u8 magic[4];
	u8 version;
	u8 bucket_bits;
	u16 header_size;
	u32 flags;
	u32 bucket_count;
	u32 record_count;
	u32 bucket_table_offset;
	u32 record_table_offset;
	u32 lengths_blob_offset;
	u32 lengths_blob_size;
	u32 record_size;
	u32 build_unix_time;
	u32 crc32;
	u8 reserved[16];
} UzesidLidxHeader;

typedef struct
{
	u8 md5[16];
	u16 subtune_count;
	u16 flags;
	u32 data_or_length;
} UzesidLidxRecord;

typedef struct
{
	UzesidReader io;
	u32 base_offset;
	u32 next_region_offset;
	UzesidLidxHeader header;
} UzesidLidx;

u8 UzesidLidxOpen(UzesidLidx *lidx, const UzesidReader *io, u32 base_offset);
u16 UzesidLidxBucketFromMd5(const UzesidLidx *lidx, const u8 md5[16]);
u8 UzesidLidxReadRecord(const UzesidLidx *lidx, u32 index, UzesidLidxRecord *rec);
u8 UzesidLidxFindRecordIndex(const UzesidLidx *lidx, const u8 md5[16], u32 *index_out);
u8 UzesidLidxGetLengthMs(const UzesidLidx *lidx, const u8 md5[16], u16 subtune_index, u32 *length_ms);

typedef struct
{
	u8 magic[4];
	u8 version;
	u8 block_shift;
	u16 dir_entry_size;
	u32 flags;
	u32 cache_bytes;
	u32 dir_entry_count;
	u32 dir_offset;
	u32 bitmap_offset;
	u32 data_offset;
	u32 total_blocks;
	u32 used_blocks;
	u32 next_alloc_hint;
	u32 live_entries;
	u32 crc32;
	u8 reserved[12];
} UzesidUsdcHeader;

typedef struct
{
	u8 state;
	u8 type;
	u16 flags;
	u8 md5[16];
	u32 usd_size;
	u32 length_ms;
	u16 subtune_index;
	u16 subtune_count;
	u16 tick_hz;
	u16 reserved0;
	u32 first_block;
	u32 block_count;
	char title[32];
	char author[32];
	char released[16];
	u32 entry_crc;
} UzesidUsdcEntry;

typedef struct
{
	UzesidReader io;
	u32 base_offset;
	UzesidUsdcHeader header;
	u32 block_size;
	u32 bitmap_size;
	u32 data_area_bytes;
} UzesidUsdc;

u8 UzesidUsdcOpen(UzesidUsdc *usdc, const UzesidReader *io, u32 base_offset);
u8 UzesidUsdcReadEntry(const UzesidUsdc *usdc, u32 index, UzesidUsdcEntry *entry);
u8 UzesidUsdcFindEntryIndex(const UzesidUsdc *usdc, const u8 md5[16], u16 subtune_index, u32 *entry_index);
u8 UzesidUsdcFindEntry(const UzesidUsdc *usdc, const u8 md5[16], u16 subtune_index, UzesidUsdcEntry *entry, u32 *entry_index);
u8 UzesidUsdcReadData(const UzesidUsdc *usdc, u32 first_block, u32 offset_in_usd, void *dst, u16 len);

u8 UzesidUsdcWriteHeader(UzesidUsdc *usdc);
u8 UzesidUsdcWriteEntry(UzesidUsdc *usdc, u32 index, const UzesidUsdcEntry *entry);
u8 UzesidUsdcFindFreeEntry(UzesidUsdc *usdc, u32 *index_out);
u8 UzesidUsdcFindFreeBlocks(const UzesidUsdc *usdc, u32 block_count, u32 *first_block_out);
u8 UzesidUsdcCommitBlocks(UzesidUsdc *usdc, u32 first_block, u32 block_count);
u8 UzesidUsdcWriteData(UzesidUsdc *usdc, u32 first_block, u32 offset_in_usd, const void *src, u16 len);

typedef struct
{
	u32 magic;
	u8 version;
	u16 tick_hz;
	u8 subtune_index;
	u8 flags;
	u32 clock_hz;
	u32 song_length_ms;
	u32 total_ticks;
	u32 data_size;
	u8 init_regs[UZESID_UZSD_INIT_REG_COUNT];
} UzesidUzsdHeader;

#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
typedef struct
{
	u8 reg[UZESID_UZSD_INIT_REG_COUNT];
	u8 val[UZESID_UZSD_INIT_REG_COUNT];
	u8 count;
	u8 ended;
} UzesidUzsdTick;
#endif

typedef struct
{
	UzesidReader io;
	u32 base_offset;
	u32 total_size;
	UzesidUzsdHeader header;
	u32 payload_offset;
	u32 cur_offset;
	u32 data_end;
	u32 ticks_done;
	u32 pending_skip;
	u8 current_regs[UZESID_UZSD_INIT_REG_COUNT]; /* v4 delta decoder state */
} UzesidUzsdStream;

int UzesidUzsdOpenFromReader(UzesidUzsdStream *st, const UzesidReader *io, u32 base_offset, u32 total_size);
int UzesidUzsdOpenFromSpi(UzesidUzsdStream *st, u32 base_offset, u32 total_size);
int UzesidUzsdOpenFromEntry(UzesidUzsdStream *st, const UzesidUsdc *usdc, const UzesidUsdcEntry *entry);
int UzesidUzsdRestart(UzesidUzsdStream *st);
#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
int UzesidUzsdNextTick(UzesidUzsdStream *st, UzesidUzsdTick *tick);
#endif
int UzesidUzsdApplyNextTickToSid(UzesidUzsdStream *st, void *sink_user, u8 *ended_out);

#ifdef __cplusplus
}
#endif

#endif
