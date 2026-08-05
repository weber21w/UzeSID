#include "Cache.h"
#ifndef UZESID_NOINLINE
#define UZESID_NOINLINE __attribute__((noinline))
#endif

extern u8 rcv_spi(void);
#include <string.h>
#include <avr/pgmspace.h>
#include <spiram.h>
extern u8 g_uzesid_workbuf[512];

/* Dedicated linear UZSD read-ahead window.  Its storage is shared with the
 * raw-capture header/register scratch because capture and stream playback never
 * execute concurrently.  This avoids 106 bytes of otherwise duplicated SRAM. */
#define UZESID_STREAM_PREFETCH UZESID_RUNTIME_SCRATCH_SIZE
#define g_uzsd_prefetch_buf g_uzesid_runtime_scratch
static const UzesidUzsdStream *g_uzsd_prefetch_owner;
static u8 g_uzsd_prefetch_valid;
static u8 g_uzsd_prefetch_pos;

/* UZSD v4 fixed delta dictionary.  Each register has seven common signed
 * changes selected from a representative SID corpus.  A compact event stores
 * the register in five bits and the dictionary slot in three bits; uncommon
 * values use slot seven followed by an absolute byte. */
static const int8_t g_uzsd_v4_delta[UZESID_UZSD_INIT_REG_COUNT][7] PROGMEM = {
    {  -6,   6,   8,  -8,  70,  55, -55 },
    {  -1,  -5,  -4,   1,  -2,  -6,  -8 },
    {  32,-128,  79, -32,  64,  40,   3 },
    {   1,  -1,-112,-111,  32, -48, -15 },
    {  -1, -64,   1,-112,  57, -56, -57 },
    {  -5,   5,  15, -15,   8,  -8,  72 },
    { -54,  54, -16,  16, 122,-122, -87 },
    {  70, -21,  21, -70, 110,-110,  19 },
    {  -2,  -1,   3,  -3,   1,  -4,  22 },
    {  32,  64,  31, -17,-128, -96,   8 },
    {   1,  -1, -15,  15,  48,  80,  -2 },
    {  -1, -64, -56,   1, -16,  73,  64 },
    {  15, -15,  -6,   6,  -9,   5,  -5 },
    {  16, -16,  75, -75,  15,-118, 103 },
    {-126, 126, -88,-125, -30,  88,  26 },
    {  -1,  -2, 119,   1,  -7,-119,  16 },
    {-128,  22,  16, 111,   8,  32, -31 },
    {  16,   1,  -1, -16,-128, 126,  -2 },
    {  -1,   1, -64, 112,-112,  64, -56 },
    {   9,  -9,  20, -20,   8,  -8,   4 },
    { -97,  97, -39,  39,  35, -35,   9 },
    {   0,   0,   0,   0,   0,   0,   0 },
    {  -1, -13, -24,  -2,  -3,-127,   4 },
    { -48, -32,-112, 112, -16,   2,  -2 },
    {  48, -48,  32, -32,  -1,  16,  96 }
};

static inline int8_t uzsd_v4_get_delta(u8 reg, u8 code){
    return (int8_t)pgm_read_byte(&g_uzsd_v4_delta[reg][code]);
}
#ifdef UZESID_HAVE_PFF
#include <petitfatfs/pff.h>
#endif


/* ---- from uzesid_io.c ---- */
u16 UzesidRd16(const u8 *p){
	return (u16)p[0] | ((u16)p[1] << 8);
}

u32 UzesidRd24(const u8 *p){
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

u32 UzesidRd32(const u8 *p){
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int UzesidReadExact(const UzesidReader *io, u32 abs_offset, void *dst, u16 len){
	if(io == 0 || io->read_at == 0)
		return -1;
	return io->read_at(io->user, abs_offset, dst, len) == 0 ? 0 : -1;
}

/* ---- from uzesid_pff_io.c ---- */
#ifdef UZESID_HAVE_PFF
u8 UzesidPffReadAt(void *user, u32 abs_offset, void *dst, u16 len){
	UINT br;
	(void)user;
	if(pf_lseek(abs_offset) != FR_OK)
		return 1;
	if(pf_read(dst, len, &br) != FR_OK)
		return 1;
	while(rcv_spi() != 0xFF);
	if(br != (UINT)len)
		return 1;
	return 0;
}

void UzesidPffInitReader(UzesidReader *io, UzesidPffContext *ctx){
	io->user = ctx;
	io->read_at = UzesidPffReadAt;
}
#endif

/* ---- SPI reader ---- */
static void uzesid_ofs_to_bank_addr(u32 ofs, u8 *bank, u16 *addr){
	*bank = (u8)(ofs >> 16);
	*addr = (u16)(ofs & 0xffffu);
}

u8 UzesidSpiReadAt(void *user, u32 abs_offset, void *dst, u16 len){
	u8 bank;
	u16 addr;
	(void)user;
	while(len != 0u)
	{
		u16 chunk = len;
		uzesid_ofs_to_bank_addr(abs_offset, &bank, &addr);
		/* 0x10000 cannot be represented by u16.  Casting the full-bank
		 * remainder when addr == 0 produced chunk == 0 and an infinite
		 * loop exactly at every 64 KiB SPI-RAM bank boundary. */
		if(addr != 0u){
			u16 room = (u16)(0x10000UL - (u32)addr);
			if(chunk > room)
				chunk = room;
		}
		SpiRamReadInto(bank, addr, dst, chunk);
		abs_offset += chunk;
		dst = (void *)((u8 *)dst + chunk);
		len = (u16)(len - chunk);
	}
	return 0;
}

void UzesidSpiInitReader(UzesidReader *io, UzesidSpiContext *ctx){
	(void)ctx;
	io->user = 0;
	io->read_at = UzesidSpiReadAt;
}

/* ---- from lidx_ro.c ---- */
static void lidx_decode_header(UzesidLidxHeader *out, const u8 raw[64]){
	memcpy(out->magic, raw + 0, 4);
	out->version = raw[4];
	out->bucket_bits = raw[5];
	out->header_size = UzesidRd16(raw + 6);
	out->flags = UzesidRd32(raw + 8);
	out->bucket_count = UzesidRd32(raw + 12);
	out->record_count = UzesidRd32(raw + 16);
	out->bucket_table_offset = UzesidRd32(raw + 20);
	out->record_table_offset = UzesidRd32(raw + 24);
	out->lengths_blob_offset = UzesidRd32(raw + 28);
	out->lengths_blob_size = UzesidRd32(raw + 32);
	out->record_size = UzesidRd32(raw + 36);
	out->build_unix_time = UzesidRd32(raw + 40);
	out->crc32 = UzesidRd32(raw + 44);
	memcpy(out->reserved, raw + 48, 16);
}

static void lidx_decode_record(UzesidLidxRecord *out, const u8 raw[24]){
	memcpy(out->md5, raw + 0, 16);
	out->subtune_count = UzesidRd16(raw + 16);
	out->flags = UzesidRd16(raw + 18);
	out->data_or_length = UzesidRd32(raw + 20);
}

u8 UzesidLidxOpen(UzesidLidx *lidx, const UzesidReader *io, u32 base_offset){
	u8 raw[64];
	u32 end_offset;

	if(lidx == 0 || io == 0)
		return 1;
	memset(lidx, 0, sizeof(*lidx));
	lidx->io = *io;
	lidx->base_offset = base_offset;
	if(UzesidReadExact(&lidx->io, base_offset, raw, (u16)sizeof(raw)) != 0)
		return 2;
	if(raw[0] != 'L' || raw[1] != 'I' || raw[2] != 'D' || raw[3] != 'X')
		return 3;
	lidx_decode_header(&lidx->header, raw);
	if(lidx->header.version != 1)
		return 4;
	if(lidx->header.header_size != 64)
		return 5;
	if(lidx->header.record_size != 24)
		return 6;
	end_offset = lidx->base_offset + lidx->header.lengths_blob_offset + lidx->header.lengths_blob_size;
	lidx->next_region_offset = (end_offset + 511UL) & ~511UL;
	return 0;
}

u16 UzesidLidxBucketFromMd5(const UzesidLidx *lidx, const u8 md5[16]){
	u8 bits;
	u16 bucket;

	if(lidx == 0 || md5 == 0)
		return 0;
	bits = lidx->header.bucket_bits;
	if(bits == 0)
		return 0;
	if(bits <= 8)
		return (u16)(md5[0] >> (8 - bits));
	bucket = (u16)(((u16)md5[0] << 8) | md5[1]);
	bucket >>= (16 - bits);
	return bucket;
}

u8 UzesidLidxReadRecord(const UzesidLidx *lidx, u32 index, UzesidLidxRecord *rec){
	u8 raw[24];
	u32 offset;

	if(lidx == 0 || rec == 0)
		return 1;
	if(index >= lidx->header.record_count)
		return 2;
	offset = lidx->base_offset + lidx->header.record_table_offset + (index * 24UL);
	if(UzesidReadExact(&lidx->io, offset, raw, (u16)sizeof(raw)) != 0)
		return 3;
	lidx_decode_record(rec, raw);
	return 0;
}

static u8 lidx_read_bucket_bounds(const UzesidLidx *lidx, u16 bucket, u32 *start, u32 *end){
	u8 raw[8];
	u32 offset;

	if(lidx == 0 || start == 0 || end == 0)
		return 1;
	if(bucket >= lidx->header.bucket_count)
		return 2;
	offset = lidx->base_offset + lidx->header.bucket_table_offset + ((u32)bucket * 4UL);
	if(UzesidReadExact(&lidx->io, offset, raw, (u16)sizeof(raw)) != 0)
		return 3;
	*start = UzesidRd32(raw + 0);
	*end = UzesidRd32(raw + 4);
	return 0;
}

u8 UzesidLidxFindRecordIndex(const UzesidLidx *lidx, const u8 md5[16], u32 *index_out){
	u16 bucket;
	u32 lo;
	u32 hi;
	UzesidLidxRecord rec;
	u8 rc;
	int cmp;

	if(lidx == 0 || md5 == 0 || index_out == 0)
		return 1;
	bucket = UzesidLidxBucketFromMd5(lidx, md5);
	rc = lidx_read_bucket_bounds(lidx, bucket, &lo, &hi);
	if(rc != 0)
		return (u8)(rc + 1);
	while(lo < hi)
	{
		u32 mid = lo + ((hi - lo) >> 1);
		rc = UzesidLidxReadRecord(lidx, mid, &rec);
		if(rc != 0)
			return (u8)(rc + 4);
		cmp = memcmp(rec.md5, md5, 16);
		if(cmp < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	if(lo >= lidx->header.record_count)
		return 8;
	rc = UzesidLidxReadRecord(lidx, lo, &rec);
	if(rc != 0)
		return (u8)(rc + 8);
	if(memcmp(rec.md5, md5, 16) != 0)
		return 12;
	*index_out = lo;
	return 0;
}

u8 UzesidLidxGetLengthMs(const UzesidLidx *lidx, const u8 md5[16], u16 subtune_index, u32 *length_ms){
	UzesidLidxRecord rec;
	u8 raw[3];
	u32 index;
	u32 offset;
	u8 rc;

	if(lidx == 0 || md5 == 0 || length_ms == 0)
		return 1;
	rc = UzesidLidxFindRecordIndex(lidx, md5, &index);
	if(rc != 0)
		return (u8)(rc + 1);
	rc = UzesidLidxReadRecord(lidx, index, &rec);
	if(rc != 0)
		return (u8)(rc + 13);
	if(subtune_index >= rec.subtune_count)
		return 17;
	if(rec.flags & UZESID_LIDX_REC_INLINE_SINGLE)
	{
		*length_ms = rec.data_or_length;
		return 0;
	}
	offset = lidx->base_offset + lidx->header.lengths_blob_offset + rec.data_or_length + ((u32)subtune_index * 3UL);
	if(UzesidReadExact(&lidx->io, offset, raw, 3) != 0)
		return 18;
	*length_ms = UzesidRd24(raw);
	return 0;
}

/* ---- from usdc_ro.c ---- */
#ifndef UZESID_DB_WRITE
#define UZESID_DB_WRITE 0
#endif
#if UZESID_DB_WRITE

static u32 usdc_crc32(const u8 *data, u16 len){
	u32 crc = 0xffffffffUL;
	u16 i;
	for(i = 0; i < len; i++){
		u8 bit;
		crc ^= data[i];
		for(bit = 0; bit < 8u; bit++)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320UL : 0u);
	}
	return crc ^ 0xffffffffUL;
}

static void wr16(u8 *p, u16 v){ p[0]=(u8)(v&0xffu); p[1]=(u8)(v>>8); }
static void wr32(u8 *p, u32 v){ p[0]=(u8)(v&0xffu); p[1]=(u8)((v>>8)&0xffu); p[2]=(u8)((v>>16)&0xffu); p[3]=(u8)((v>>24)&0xffu); }
#endif

static void usdc_decode_header(UzesidUsdcHeader *out, const u8 raw[64]){
	memcpy(out->magic, raw + 0, 4);
	out->version = raw[4];
	out->block_shift = raw[5];
	out->dir_entry_size = UzesidRd16(raw + 6);
	out->flags = UzesidRd32(raw + 8);
	out->cache_bytes = UzesidRd32(raw + 12);
	out->dir_entry_count = UzesidRd32(raw + 16);
	out->dir_offset = UzesidRd32(raw + 20);
	out->bitmap_offset = UzesidRd32(raw + 24);
	out->data_offset = UzesidRd32(raw + 28);
	out->total_blocks = UzesidRd32(raw + 32);
	out->used_blocks = UzesidRd32(raw + 36);
	out->next_alloc_hint = UzesidRd32(raw + 40);
	out->live_entries = UzesidRd32(raw + 44);
	out->crc32 = UzesidRd32(raw + 48);
	memcpy(out->reserved, raw + 52, 12);
}

#if UZESID_DB_WRITE
static void usdc_encode_header(const UzesidUsdcHeader *in, u8 raw[64]){
	memset(raw, 0, 64);
	memcpy(raw + 0, in->magic, 4);
	raw[4] = in->version;
	raw[5] = in->block_shift;
	wr16(raw + 6, in->dir_entry_size);
	wr32(raw + 8, in->flags);
	wr32(raw + 12, in->cache_bytes);
	wr32(raw + 16, in->dir_entry_count);
	wr32(raw + 20, in->dir_offset);
	wr32(raw + 24, in->bitmap_offset);
	wr32(raw + 28, in->data_offset);
	wr32(raw + 32, in->total_blocks);
	wr32(raw + 36, in->used_blocks);
	wr32(raw + 40, in->next_alloc_hint);
	wr32(raw + 44, in->live_entries);
	wr32(raw + 48, in->crc32);
	memcpy(raw + 52, in->reserved, 12);
}
#endif

static void usdc_decode_entry(UzesidUsdcEntry *out, const u8 raw[128]){
	memset(out, 0, sizeof(*out));
	out->state = raw[0];
	out->type = raw[1];
	out->flags = UzesidRd16(raw + 2);
	memcpy(out->md5, raw + 4, 16);
	out->usd_size = UzesidRd32(raw + 20);
	out->length_ms = UzesidRd32(raw + 24);
	out->subtune_index = UzesidRd16(raw + 28);
	out->subtune_count = UzesidRd16(raw + 30);
	out->tick_hz = UzesidRd16(raw + 32);
	out->reserved0 = UzesidRd16(raw + 34);
	out->first_block = UzesidRd32(raw + 36);
	out->block_count = UzesidRd32(raw + 40);
	memcpy(out->title, raw + 44, 32);
	memcpy(out->author, raw + 76, 32);
	memcpy(out->released, raw + 108, 16);
	out->entry_crc = UzesidRd32(raw + 124);
	out->title[31] = 0;
	out->author[31] = 0;
	out->released[15] = 0;
}

#if UZESID_DB_WRITE
static void usdc_encode_entry(const UzesidUsdcEntry *in, u8 raw[128]){
	memset(raw, 0, 128);
	raw[0] = in->state;
	raw[1] = in->type;
	wr16(raw + 2, in->flags);
	memcpy(raw + 4, in->md5, 16);
	wr32(raw + 20, in->usd_size);
	wr32(raw + 24, in->length_ms);
	wr16(raw + 28, in->subtune_index);
	wr16(raw + 30, in->subtune_count);
	wr16(raw + 32, in->tick_hz);
	wr16(raw + 34, in->reserved0);
	wr32(raw + 36, in->first_block);
	wr32(raw + 40, in->block_count);
	memcpy(raw + 44, in->title, 32);
	memcpy(raw + 76, in->author, 32);
	memcpy(raw + 108, in->released, 16);
	wr32(raw + 124, in->entry_crc);
}
#endif

#if UZESID_DB_WRITE && UZESID_HAVE_PFF
static UZESID_NOINLINE u8 usdc_write_exact(u32 abs_offset, const void *src, u16 len){
	u8 *secbuf = g_uzesid_workbuf;
	const u8 *sp = (const u8 *)src;
	UINT br;
	UINT bw;
	u32 sector_base;
	u16 sector_off;
	u16 chunk;
	if(src == 0) return 1;
	while(len){
		sector_base = abs_offset & ~511UL;
		sector_off = (u16)(abs_offset & 511UL);
		chunk = (u16)(512U - sector_off);
		if(chunk > len) chunk = len;
		if(chunk != 512U){
			if(pf_lseek(sector_base) != FR_OK) return 2;
			if(pf_read(secbuf, 512, &br) != FR_OK) return 3;
			while(rcv_spi() != 0xFF);
			if(br != 512U) return 4;
			memcpy(secbuf + sector_off, sp, chunk);
			if(pf_lseek(sector_base) != FR_OK) return 5;
			if(pf_write(secbuf, 512, &bw) != FR_OK) return 6;
			if(bw != 512U) return 7;
			if(pf_write(0, 0, &bw) != FR_OK) return 8;
		}else{
			if(pf_lseek(sector_base) != FR_OK) return 9;
			if(pf_write(sp, 512, &bw) != FR_OK) return 10;
			if(bw != 512U) return 11;
			if(pf_write(0, 0, &bw) != FR_OK) return 12;
		}
		abs_offset += chunk;
		sp += chunk;
		len = (u16)(len - chunk);
	}
	return 0;
}
#elif UZESID_DB_WRITE
static u8 usdc_write_exact(u32 abs_offset, const void *src, u16 len){
	(void)abs_offset;
	(void)src;
	(void)len;
	return 1;
}
#endif

u8 UzesidUsdcOpen(UzesidUsdc *usdc, const UzesidReader *io, u32 base_offset){
	u8 raw[64];

	if(usdc == 0 || io == 0)
		return 1;
	memset(usdc, 0, sizeof(*usdc));
	usdc->io = *io;
	usdc->base_offset = base_offset;
	if(UzesidReadExact(&usdc->io, base_offset, raw, (u16)sizeof(raw)) != 0)
		return 2;
	if(raw[0] != 'U' || raw[1] != 'S' || raw[2] != 'D' || raw[3] != 'C')
		return 3;
	usdc_decode_header(&usdc->header, raw);
	if(usdc->header.version != 1)
		return 4;
	if(usdc->header.dir_entry_size != 128)
		return 5;
	usdc->block_size = 1UL << usdc->header.block_shift;
	usdc->bitmap_size = (usdc->header.total_blocks + 7UL) >> 3;
	if(usdc->header.cache_bytes < usdc->header.data_offset)
		return 6;
	usdc->data_area_bytes = usdc->header.cache_bytes - usdc->header.data_offset;
	return 0;
}

UZESID_NOINLINE u8 UzesidUsdcReadEntry(const UzesidUsdc *usdc, u32 index, UzesidUsdcEntry *entry){
	u8 raw[128];
	u32 offset;

	if(usdc == 0 || entry == 0)
		return 1;
	if(index >= usdc->header.dir_entry_count)
		return 2;
	offset = usdc->base_offset + usdc->header.dir_offset + (index * 128UL);
	if(UzesidReadExact(&usdc->io, offset, raw, (u16)sizeof(raw)) != 0)
		return 3;
	usdc_decode_entry(entry, raw);
	return 0;
}

static UZESID_NOINLINE u8 usdc_entry_key_matches(const UzesidUsdc *usdc, u32 index,
	const u8 md5[16], u16 subtune_index, u8 *matches){
	u8 raw[32];
	u32 offset;
	if(usdc == 0 || md5 == 0 || matches == 0)
		return 1;
	offset = usdc->base_offset + usdc->header.dir_offset + (index * 128UL);
	if(UzesidReadExact(&usdc->io, offset, raw, (u16)sizeof(raw)) != 0)
		return 2;
	*matches = (u8)(raw[0] == UZESID_USDC_ENTRY_LIVE &&
		raw[1] == UZESID_USDC_ENTRY_TYPE_USD &&
		UzesidRd16(raw + 28) == subtune_index &&
		memcmp(raw + 4, md5, 16) == 0);
	return 0;
}

UZESID_NOINLINE u8 UzesidUsdcFindEntryIndex(const UzesidUsdc *usdc, const u8 md5[16], u16 subtune_index, u32 *entry_index){
	u32 i;
	u8 matches;
	u8 rc;

	if(usdc == 0 || md5 == 0 || entry_index == 0)
		return 1;
	for(i = 0; i < usdc->header.dir_entry_count; i++){
		rc = usdc_entry_key_matches(usdc, i, md5, subtune_index, &matches);
		if(rc != 0)
			return (u8)(rc + 1);
		if(matches){
			*entry_index = i;
			return 0;
		}
	}
	return 5;
}

UZESID_NOINLINE u8 UzesidUsdcFindEntry(const UzesidUsdc *usdc, const u8 md5[16], u16 subtune_index, UzesidUsdcEntry *entry, u32 *entry_index){
	u32 idx;
	u8 rc;

	if(usdc == 0 || md5 == 0 || entry == 0)
		return 1;
	rc = UzesidUsdcFindEntryIndex(usdc, md5, subtune_index, &idx);
	if(rc != 0)
		return (u8)(rc + 1);
	rc = UzesidUsdcReadEntry(usdc, idx, entry);
	if(rc != 0)
		return (u8)(rc + 6);
	if(entry_index != 0)
		*entry_index = idx;
	return 0;
}

u8 UzesidUsdcReadData(const UzesidUsdc *usdc, u32 first_block, u32 offset_in_usd, void *dst, u16 len){
	u32 abs_offset;

	if(usdc == 0 || dst == 0)
		return 1;
	abs_offset = usdc->base_offset + usdc->header.data_offset + (first_block << usdc->header.block_shift) + offset_in_usd;
	if(UzesidReadExact(&usdc->io, abs_offset, dst, len) != 0)
		return 2;
	return 0;
}

#if UZESID_DB_WRITE
UZESID_NOINLINE u8 UzesidUsdcWriteHeader(UzesidUsdc *usdc){
	u8 raw[64];
	if(usdc == 0)
		return 1;
	/* The CRC covers the complete serialized header with its own field zero. */
	usdc->header.crc32 = 0;
	usdc_encode_header(&usdc->header, raw);
	usdc->header.crc32 = usdc_crc32(raw, (u16)sizeof(raw));
	wr32(raw + 48, usdc->header.crc32);
	if(usdc_write_exact(usdc->base_offset, raw, (u16)sizeof(raw)) != 0)
		return 2;
	return 0;
}

UZESID_NOINLINE u8 UzesidUsdcWriteEntry(UzesidUsdc *usdc, u32 index, const UzesidUsdcEntry *entry){
	u8 raw[128];
	u32 offset;
	u32 crc;
	if(usdc == 0 || entry == 0)
		return 1;
	if(index >= usdc->header.dir_entry_count)
		return 2;
	usdc_encode_entry(entry, raw);
	/* Entry CRC uses the serialized record with the CRC field cleared. */
	memset(raw + 124, 0, 4);
	crc = usdc_crc32(raw, (u16)sizeof(raw));
	wr32(raw + 124, crc);
	offset = usdc->base_offset + usdc->header.dir_offset + (index * 128UL);
	if(usdc_write_exact(offset, raw, (u16)sizeof(raw)) != 0)
		return 3;
	return 0;
}

UZESID_NOINLINE u8 UzesidUsdcFindFreeEntry(UzesidUsdc *usdc, u32 *index_out){
	u32 i;
	u32 offset;
	u8 state;
	if(usdc == 0 || index_out == 0)
		return 1;
	for(i = 0; i < usdc->header.dir_entry_count; i++){
		offset = usdc->base_offset + usdc->header.dir_offset + (i * 128UL);
		if(UzesidReadExact(&usdc->io, offset, &state, 1) != 0)
			return 2;
		if(state == UZESID_USDC_ENTRY_FREE || state == UZESID_USDC_ENTRY_DELETED){
			*index_out = i;
			return 0;
		}
	}
	return 5;
}


static u8 bitmap_set_range(UzesidUsdc *usdc, u32 first, u32 count){
	while(count != 0u){
		u8 b;
		u8 first_bit = (u8)(first & 7u);
		u8 n = (u8)(8u - first_bit);
		u8 mask;
		u32 abs;
		if((u32)n > count) n = (u8)count;
		abs = usdc->base_offset + usdc->header.bitmap_offset + (first >> 3);
		if(UzesidReadExact(&usdc->io, abs, &b, 1) != 0) return 1;
		mask = (u8)(((1u << n) - 1u) << first_bit);
		b |= mask;
		if(usdc_write_exact(abs, &b, 1) != 0) return 2;
		first += n;
		count -= n;
	}
	return 0;
}

static UZESID_NOINLINE u8 bitmap_find_free_run(const UzesidUsdc *usdc,
	u32 begin, u32 end, u32 needed, u32 *first_block_out){
	u32 pos = begin;
	u32 run_start = 0;
	u32 run_length = 0;
	while(pos < end){
		u32 byte_index = pos >> 3;
		u32 final_byte = (end + 7UL) >> 3;
		u16 bytes = (u16)(final_byte - byte_index);
		u16 i;
		if(bytes > 64u) bytes = 64u;
		if(UzesidReadExact(&usdc->io,
			usdc->base_offset + usdc->header.bitmap_offset + byte_index,
			g_uzesid_workbuf, bytes) != 0)
			return 1;
		for(i = 0; i < bytes && pos < end; i++){
			u8 bits = g_uzesid_workbuf[i];
			u8 bit = (u8)(pos & 7u);
			for(; bit < 8u && pos < end; bit++, pos++){
				if((bits & (u8)(1u << bit)) == 0u){
					if(run_length == 0u) run_start = pos;
					run_length++;
					if(run_length == needed){
						*first_block_out = run_start;
						return 0;
					}
				}else{
					run_length = 0;
				}
			}
		}
	}
	return 9;
}

UZESID_NOINLINE u8 UzesidUsdcFindFreeBlocks(const UzesidUsdc *usdc, u32 block_count, u32 *first_block_out){
	u8 rc;
	if(usdc == 0 || first_block_out == 0 || block_count == 0)
		return 1;
	if(block_count > usdc->header.total_blocks)
		return 2;
	/* Scan the allocation bitmap in 64-byte sequential chunks.  A 128 MiB
	 * database has about 4 KiB of bitmap; reading one bit with a fresh Petit
	 * FatFs seek made an out-of-space check take tens of thousands of seeks. */
	rc = bitmap_find_free_run(usdc, usdc->header.next_alloc_hint,
		usdc->header.total_blocks, block_count, first_block_out);
	if(rc == 0) return 0;
	if(rc != 9) return (u8)(rc + 2);
	if(usdc->header.next_alloc_hint != 0u){
		rc = bitmap_find_free_run(usdc, 0u, usdc->header.next_alloc_hint,
			block_count, first_block_out);
		if(rc == 0) return 0;
		if(rc != 9) return (u8)(rc + 2);
	}
	return 9;
}

UZESID_NOINLINE u8 UzesidUsdcCommitBlocks(UzesidUsdc *usdc, u32 first_block, u32 block_count){
	if(usdc == 0 || block_count == 0)
		return 1;
	if(first_block >= usdc->header.total_blocks ||
		block_count > usdc->header.total_blocks - first_block)
		return 2;
	if(bitmap_set_range(usdc, first_block, block_count) != 0)
		return 3;
	usdc->header.used_blocks += block_count;
	usdc->header.next_alloc_hint = first_block + block_count;
	if(usdc->header.next_alloc_hint >= usdc->header.total_blocks)
		usdc->header.next_alloc_hint = 0;
	return 0;
}

UZESID_NOINLINE u8 UzesidUsdcWriteData(UzesidUsdc *usdc, u32 first_block, u32 offset_in_usd, const void *src, u16 len){
	u32 abs_offset;
	if(usdc == 0 || src == 0)
		return 1;
	abs_offset = usdc->base_offset + usdc->header.data_offset + (first_block << usdc->header.block_shift) + offset_in_usd;
	if(usdc_write_exact(abs_offset, src, len) != 0)
		return 2;
	return 0;
}
#else
u8 UzesidUsdcWriteHeader(UzesidUsdc *usdc){ (void)usdc; return 1; }
u8 UzesidUsdcWriteEntry(UzesidUsdc *usdc, u32 index, const UzesidUsdcEntry *entry){ (void)usdc; (void)index; (void)entry; return 1; }
u8 UzesidUsdcFindFreeEntry(UzesidUsdc *usdc, u32 *index_out){ (void)usdc; (void)index_out; return 1; }
u8 UzesidUsdcFindFreeBlocks(const UzesidUsdc *usdc, u32 block_count, u32 *first_block_out){ (void)usdc; (void)block_count; (void)first_block_out; return 1; }
u8 UzesidUsdcCommitBlocks(UzesidUsdc *usdc, u32 first_block, u32 block_count){ (void)usdc; (void)first_block; (void)block_count; return 1; }
u8 UzesidUsdcWriteData(UzesidUsdc *usdc, u32 first_block, u32 offset_in_usd, const void *src, u16 len){ (void)usdc; (void)first_block; (void)offset_in_usd; (void)src; (void)len; return 1; }
#endif

/* ---- from uzsd_ro.c ---- */
#if !defined(UZESID_PLAYBACK_SPI_ONLY) || !(UZESID_PLAYBACK_SPI_ONLY)
static int uzsd_read_abs_entry_fallback(const UzesidUzsdStream *st, u32 offset_in_uzsd, void *dst, u16 len);
#endif

static void uzsd_prefetch_invalidate(void){
	g_uzsd_prefetch_owner = 0;
	g_uzsd_prefetch_valid = 0;
	g_uzsd_prefetch_pos = 0;
}

/* Fill a persistent linear window only when the requested stream position is
 * outside the current window.  A 192-byte fill usually covers many 50 Hz
 * register ticks, so playback no longer starts one SPI transaction per tick. */
static int uzsd_prefetch_fill(const UzesidUzsdStream *st, u32 offset){
	u32 remaining;
	u16 want;

	if(st == 0 || st->io.read_at == 0)
		return 0;
	if(offset >= st->data_end){
		uzsd_prefetch_invalidate();
		return 0;
	}
	remaining = st->data_end - offset;
	want = (remaining > UZESID_STREAM_PREFETCH) ?
		UZESID_STREAM_PREFETCH : (u16)remaining;
	g_uzsd_prefetch_owner = 0;
	g_uzsd_prefetch_valid = 0;
	if(UzesidReadExact(&st->io, st->base_offset + offset,
			g_uzsd_prefetch_buf, want) != 0)
		return -1;
	g_uzsd_prefetch_valid = (u8)want;
	g_uzsd_prefetch_pos = 0;
	g_uzsd_prefetch_owner = st;
	return 0;
}

static int uzsd_read_u8(UzesidUzsdStream *st, u8 *out){
#if !defined(UZESID_PLAYBACK_SPI_ONLY) || !(UZESID_PLAYBACK_SPI_ONLY)
	if(st->io.read_at == 0){
		if(uzsd_read_abs_entry_fallback(st, st->cur_offset, out, 1) != 0)
			return -1;
		st->cur_offset++;
		return 0;
	}
#endif
	/* Playback is strictly linear between restart/loop operations.  Keep a
	 * byte cursor inside the persistent window so the hot getc path avoids
	 * 32-bit range comparisons and subtraction for every encoded byte. */
	if(g_uzsd_prefetch_owner != st ||
		g_uzsd_prefetch_pos >= g_uzsd_prefetch_valid){
		if(st->cur_offset >= st->data_end ||
			uzsd_prefetch_fill(st, st->cur_offset) != 0 ||
			g_uzsd_prefetch_valid == 0u)
			return -1;
	}
	*out = g_uzsd_prefetch_buf[g_uzsd_prefetch_pos++];
	st->cur_offset++;
	return 0;
}

void UzesidUzsdIdlePrefetch(UzesidUzsdStream *st){
	u8 remain;
	u16 want;
	u32 fill_offset;
	u32 available;

	/* Compact the unread tail and refill the window while the main loop would
	 * otherwise be waiting for vsync.  Cached playback remains byte-for-byte
	 * identical; this only moves the SPI burst out of the audio hot path. */
	if(st == 0 || st->io.read_at == 0 ||
		g_uzsd_prefetch_owner != st ||
		g_uzsd_prefetch_pos < (UZESID_STREAM_PREFETCH / 2u))
		return;

	/* Refill only after at least half the window was consumed. The previous
	 * every-frame compaction repeatedly moved almost the entire unread tail,
	 * wasting the idle time now reserved for smooth GUI updates. */
	remain = (u8)(g_uzsd_prefetch_valid - g_uzsd_prefetch_pos);
	if(remain != 0u)
		memmove(g_uzsd_prefetch_buf,
			g_uzsd_prefetch_buf + g_uzsd_prefetch_pos, remain);
	g_uzsd_prefetch_valid = remain;
	g_uzsd_prefetch_pos = 0u;

	fill_offset = st->cur_offset + remain;
	if(fill_offset >= st->data_end)
		return;
	available = st->data_end - fill_offset;
	want = (u16)(UZESID_STREAM_PREFETCH - remain);
	if((u32)want > available)
		want = (u16)available;
	if(want != 0u){
		if(UzesidReadExact(&st->io, st->base_offset + fill_offset,
				g_uzsd_prefetch_buf + remain, want) != 0){
			uzsd_prefetch_invalidate();
			return;
		}
		g_uzsd_prefetch_valid = (u8)(remain + want);
	}
}

static int uzsd_read_uleb128(UzesidUzsdStream *st, u32 *value){
	u32 v = 0;
	u8 shift = 0;
	u8 b;

	if(value == 0)
		return -1;
	for(;;)
	{
		if(uzsd_read_u8(st, &b) != 0)
			return -1;
		v |= ((u32)(b & 0x7fu) << shift);
		if((b & 0x80u) == 0)
			break;
		shift += 7;
		if(shift > 28)
			return -1;
	}
	*value = v;
	return 0;
}

static int uzsd_decode_header(UzesidUzsdHeader *hdr, const u8 raw[UZESID_UZSD_HEADER_SIZE]){
	memset(hdr, 0, sizeof(*hdr));
	hdr->magic = UzesidRd32(raw + 0);
	hdr->version = raw[4];
	hdr->tick_hz = (u16)raw[5] |
		((u16)(raw[7] >> UZESID_UZSD_TICK_HI_SHIFT) << 8);
	hdr->subtune_index = raw[6];
	hdr->flags = (u8)(raw[7] & UZESID_UZSD_FLAG_MASK);
	hdr->clock_hz = UzesidRd32(raw + 8);
	hdr->song_length_ms = UzesidRd32(raw + 12);
	hdr->total_ticks = UzesidRd32(raw + 16);
	hdr->data_size = UzesidRd32(raw + 20);
	memcpy(hdr->init_regs, raw + 24, UZESID_UZSD_INIT_REG_COUNT);
	if(hdr->magic != UZESID_UZSD_MAGIC ||
	   (hdr->version != UZESID_UZSD_VERSION_V3 && hdr->version != UZESID_UZSD_VERSION_V4))
		return -1;
	return 0;
}

int UzesidUzsdOpenFromReader(UzesidUzsdStream *st, const UzesidReader *io, u32 base_offset, u32 total_size){
	u8 raw[UZESID_UZSD_HEADER_SIZE];

	if(st == 0 || io == 0)
		return -1;
	if(total_size != 0u && total_size < UZESID_UZSD_HEADER_SIZE)
		return -1;
	memset(st, 0, sizeof(*st));
	st->io = *io;
	st->base_offset = base_offset;
	st->total_size = total_size;
	if(UzesidReadExact(&st->io, base_offset, raw, (u16)sizeof(raw)) != 0)
		return -1;
	if(uzsd_decode_header(&st->header, raw) != 0)
		return -1;
	st->payload_offset = UZESID_UZSD_HEADER_SIZE;
	st->cur_offset = st->payload_offset;
	st->data_end = st->payload_offset + st->header.data_size;
	if(total_size != 0u && st->data_end > total_size)
		return -1;
	return 0;
}

int UzesidUzsdOpenFromSpi(UzesidUzsdStream *st, u32 base_offset, u32 total_size){
	UzesidReader io;
	UzesidSpiInitReader(&io, 0);
	return UzesidUzsdOpenFromReader(st, &io, base_offset, total_size);
}

int UzesidUzsdOpenFromEntry(UzesidUzsdStream *st, const UzesidUsdc *usdc, const UzesidUsdcEntry *entry){
	if(st == 0 || usdc == 0 || entry == 0)
		return -1;
	/* Wrap USDC entry data as a generic absolute reader via UzesidUsdcReadData. */
	memset(st, 0, sizeof(*st));
	st->base_offset = 0;
	st->total_size = entry->usd_size;
	if(entry->usd_size < UZESID_UZSD_HEADER_SIZE)
		return -1;
	u8 raw[UZESID_UZSD_HEADER_SIZE];
	if(UzesidUsdcReadData(usdc, entry->first_block, 0, raw, (u16)sizeof(raw)) != 0)
		return -1;
	if(uzsd_decode_header(&st->header, raw) != 0)
		return -1;
	/* Reuse io/base_offset fields through an internal trampoline. */
	st->io.user = (void *)usdc;
	st->io.read_at = 0;
	st->payload_offset = UZESID_UZSD_HEADER_SIZE;
	st->cur_offset = st->payload_offset;
	st->data_end = st->payload_offset + st->header.data_size;
	if(st->data_end > entry->usd_size)
		return -1;
	/* Store block info in base_offset/total_size is not enough, so use a private encoding. */
	st->base_offset = entry->first_block;
	st->total_size = entry->usd_size;
	return 0;
}

#if !defined(UZESID_PLAYBACK_SPI_ONLY) || !(UZESID_PLAYBACK_SPI_ONLY)
static int uzsd_read_abs_entry_fallback(const UzesidUzsdStream *st, u32 offset_in_uzsd, void *dst, u16 len){
	const UzesidUsdc *usdc = (const UzesidUsdc *)st->io.user;
	return UzesidUsdcReadData(usdc, st->base_offset, offset_in_uzsd, dst, len);
}
#endif

int UzesidUzsdRestart(UzesidUzsdStream *st){
	if(st == 0)
		return -1;
	st->cur_offset = st->payload_offset;
	st->ticks_done = 0;
	st->pending_skip = 0;
	memcpy(st->current_regs, st->header.init_regs, UZESID_UZSD_INIT_REG_COUNT);
	uzsd_prefetch_invalidate();
	return 0;
}


/* Direct playback sink implemented by UzeSID.c.  Keeping this path here lets
 * the decoder apply a tick as it is read, avoiding the 52-byte temporary
 * UzesidUzsdTick and a second register-copy pass. */
extern void SIDBeginRegisterBatch(void);
extern void SIDWriteRegister(u8 reg, u8 val);
extern void SIDEndRegisterBatch(void);

static int uzsd_v4_apply_next_tick_to_sid(UzesidUzsdStream *st, void *sink_user, u8 *ended_out){
	(void)sink_user;
	u8 batching = 0;
	for(;;)
	{
		u8 token, reg, code, val;
		/* Valid v4 streams terminate by total_ticks or an END token. Let the
		 * byte reader detect a truncated payload only when its window empties,
		 * instead of doing a 32-bit offset comparison for every register token. */
		if(uzsd_read_u8(st, &token) != 0)
		{
			if(batching) SIDEndRegisterBatch();
			return -1;
		}
		reg = (u8)(token & 31u);
		code = (u8)(token >> 5);
		if(reg < UZESID_UZSD_INIT_REG_COUNT)
		{
			if(code == 7u)
			{
				if(uzsd_read_u8(st, &val) != 0)
				{
					if(batching) SIDEndRegisterBatch();
					return -1;
				}
			}
			else
				val = (u8)(st->current_regs[reg] + uzsd_v4_get_delta(reg, code));
			st->current_regs[reg] = val;
			if(!batching)
			{
				SIDBeginRegisterBatch();
				batching = 1;
			}
			SIDWriteRegister(reg, val);
			continue;
		}
		if(reg == UZESID_UZSD_V4_CTRL_END_TICK && code == 0u)
		{
			if(batching) SIDEndRegisterBatch();
			st->ticks_done++;
			return 0;
		}
		if(reg == UZESID_UZSD_V4_CTRL_SKIP && code == 0u && !batching)
		{
			u32 run;
			if(uzsd_read_uleb128(st, &run) != 0) return -1;
			if(run == 0u) run = 1u;
			st->ticks_done++;
			st->pending_skip = run - 1u;
			return 0;
		}
		if(reg == UZESID_UZSD_V4_CTRL_END && code == 0u && !batching)
		{
			if(ended_out != 0) *ended_out = 1;
			return 1;
		}
		if(batching) SIDEndRegisterBatch();
		return -1;
	}
}

int UzesidUzsdApplyNextTickToSid(UzesidUzsdStream *st, void *sink_user, u8 *ended_out){
	u8 op;
	u8 i;

	if(ended_out != 0)
		*ended_out = 0;
	if(st == 0)
		return -1;
	if(st->ticks_done >= st->header.total_ticks)
	{
		if(ended_out != 0) *ended_out = 1;
		return 1;
	}
	if(st->pending_skip != 0)
	{
		st->pending_skip--;
		st->ticks_done++;
		return 0;
	}
	if(st->cur_offset >= st->data_end)
	{
		if(ended_out != 0) *ended_out = 1;
		return 1;
	}
	if(st->header.version == UZESID_UZSD_VERSION_V4)
		return uzsd_v4_apply_next_tick_to_sid(st, sink_user, ended_out);
	if(uzsd_read_u8(st, &op) != 0)
		return -1;

	switch(op)
	{
		case UZESID_UZSD_OP_SKIP:
		{
			u32 run;
			if(uzsd_read_uleb128(st, &run) != 0)
				return -1;
			if(run == 0) run = 1;
			st->ticks_done++;
			st->pending_skip = run - 1;
			return 0;
		}

		case UZESID_UZSD_OP_PAIRS:
		{
			u8 count;
			if(uzsd_read_u8(st, &count) != 0 || count > UZESID_UZSD_INIT_REG_COUNT)
				return -1;
			if(count != 0) SIDBeginRegisterBatch();
			for(i = 0; i < count; i++)
			{
				u8 reg, val;
				if(uzsd_read_u8(st, &reg) != 0 || uzsd_read_u8(st, &val) != 0)
				{
					if(count != 0) SIDEndRegisterBatch();
					return -1;
				}
				SIDWriteRegister(reg, val);
			}
			if(count != 0) SIDEndRegisterBatch();
			st->ticks_done++;
			return 0;
		}

		case UZESID_UZSD_OP_MASK:
		{
			u8 mask[4];
			u8 any;
			if(uzsd_read_u8(st, &mask[0]) != 0 ||
			   uzsd_read_u8(st, &mask[1]) != 0 ||
			   uzsd_read_u8(st, &mask[2]) != 0 ||
			   uzsd_read_u8(st, &mask[3]) != 0)
				return -1;
			any = (u8)(mask[0] | mask[1] | mask[2] | (mask[3] & 1u));
			if(any != 0) SIDBeginRegisterBatch();
			for(i = 0; i < UZESID_UZSD_INIT_REG_COUNT; i++)
			{
				u8 bit = (u8)(1u << (i & 7u));
				if(mask[i >> 3] & bit)
				{
					u8 val;
					if(uzsd_read_u8(st, &val) != 0)
					{
						if(any != 0) SIDEndRegisterBatch();
						return -1;
					}
					SIDWriteRegister(i, val);
				}
			}
			if(any != 0) SIDEndRegisterBatch();
			st->ticks_done++;
			return 0;
		}

		case UZESID_UZSD_OP_END:
			if(ended_out != 0) *ended_out = 1;
			return 1;
		default:
			return -1;
	}
}

#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
static int uzsd_v4_next_tick(UzesidUzsdStream *st, UzesidUzsdTick *tick){
	for(;;)
	{
		u8 token, reg, code, val;
		if(st->cur_offset >= st->data_end) { tick->ended = 1; return 1; }
		if(uzsd_read_u8(st, &token) != 0) return -1;
		reg = (u8)(token & 31u);
		code = (u8)(token >> 5);
		if(reg < UZESID_UZSD_INIT_REG_COUNT)
		{
			if(tick->count >= UZESID_UZSD_INIT_REG_COUNT) return -1;
			if(code == 7u) { if(uzsd_read_u8(st, &val) != 0) return -1; }
			else val = (u8)(st->current_regs[reg] + uzsd_v4_get_delta(reg, code));
			st->current_regs[reg] = val;
			tick->reg[tick->count] = reg;
			tick->val[tick->count] = val;
			tick->count++;
			continue;
		}
		if(reg == UZESID_UZSD_V4_CTRL_END_TICK && code == 0u)
		{
			st->ticks_done++;
			return 0;
		}
		if(reg == UZESID_UZSD_V4_CTRL_SKIP && code == 0u && tick->count == 0u)
		{
			u32 run;
			if(uzsd_read_uleb128(st, &run) != 0) return -1;
			if(run == 0u) run = 1u;
			st->ticks_done++;
			st->pending_skip = run - 1u;
			return 0;
		}
		if(reg == UZESID_UZSD_V4_CTRL_END && code == 0u && tick->count == 0u)
		{
			tick->ended = 1;
			return 1;
		}
		return -1;
	}
}

int UzesidUzsdNextTick(UzesidUzsdStream *st, UzesidUzsdTick *tick){
	u8 op;
	u8 i;

	if(st == 0 || tick == 0)
		return -1;
	memset(tick, 0, sizeof(*tick));
	if(st->ticks_done >= st->header.total_ticks)
	{
		tick->ended = 1;
		return 1;
	}
	if(st->pending_skip != 0)
	{
		st->pending_skip--;
		st->ticks_done++;
		return 0;
	}
	if(st->cur_offset >= st->data_end)
	{
		tick->ended = 1;
		return 1;
	}
	if(st->header.version == UZESID_UZSD_VERSION_V4)
		return uzsd_v4_next_tick(st, tick);
	if(uzsd_read_u8(st, &op) != 0)
		return -1;
	switch(op)
	{
		case UZESID_UZSD_OP_SKIP:
		{
			u32 run;
			if(uzsd_read_uleb128(st, &run) != 0)
				return -1;
			if(run == 0)
				run = 1;
			st->ticks_done++;
			st->pending_skip = run - 1;
			return 0;
		}
		case UZESID_UZSD_OP_PAIRS:
		{
			u8 count;
			if(uzsd_read_u8(st, &count) != 0)
				return -1;
			if(count > UZESID_UZSD_INIT_REG_COUNT)
				return -1;
			for(i = 0; i < count; i++)
			{
				if(uzsd_read_u8(st, &tick->reg[i]) != 0)
					return -1;
				if(uzsd_read_u8(st, &tick->val[i]) != 0)
					return -1;
			}
			tick->count = count;
			st->ticks_done++;
			return 0;
		}
		case UZESID_UZSD_OP_MASK:
		{
			u8 raw_mask[4];
			u32 mask;
			if(uzsd_read_u8(st, &raw_mask[0]) != 0 ||
			   uzsd_read_u8(st, &raw_mask[1]) != 0 ||
			   uzsd_read_u8(st, &raw_mask[2]) != 0 ||
			   uzsd_read_u8(st, &raw_mask[3]) != 0)
				return -1;
			mask = UzesidRd32(raw_mask);
			for(i = 0; i < UZESID_UZSD_INIT_REG_COUNT; i++)
			{
				if(mask & (1UL << i))
				{
					if(uzsd_read_u8(st, &tick->val[tick->count]) != 0)
						return -1;
					tick->reg[tick->count] = i;
					tick->count++;
				}
			}
			st->ticks_done++;
			return 0;
		}
		case UZESID_UZSD_OP_END:
			tick->ended = 1;
			return 1;
		default:
			return -1;
	}
}
#endif /* generic UZSD tick API */
