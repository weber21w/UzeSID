#include "Emulation.h"
#ifndef UZESID_NOINLINE
#define UZESID_NOINLINE __attribute__((noinline))
#endif

extern u8 rcv_spi(void);
extern void UzeSidLoadProgress(u8 stage);
extern void UzeSidCaptureProgress(u32 captured_ticks, u32 total_ticks);
extern void SIDSetClockHz(u32 clock_hz);
#include <string.h>
#include <avr/pgmspace.h>
#include <spiram.h>
#ifdef UZESID_HAVE_PFF
#include <petitfatfs/pff.h>
extern FATFS fs;
#endif

#define PSID_MIN_HEADER_LENGTH 0x76
#define PSID_MAX_HEADER_LENGTH 0x7c
#define PSID_ID        0x00
#define PSID_VERSION   0x04
#define PSID_LENGTH    0x06
#define PSID_START     0x08
#define PSID_INIT      0x0a
#define PSID_MAIN      0x0c
#define PSID_NUMBER    0x0e
#define PSID_DEFSONG   0x10
#define PSID_SPEED     0x12
#define PSID_NAME      0x16
#define PSID_AUTHOR    0x36
#define PSID_COPYRIGHT 0x56
#define PSID_FLAGS     0x76

#ifndef UZESID_ENABLE_ILLEGAL_OPS
#define UZESID_ENABLE_ILLEGAL_OPS 1
#endif

/* Raw import/capture state moved to the emulation module. */
u8 g_uzesid_capture_enabled;
u8 g_uzesid_runtime_scratch[UZESID_RUNTIME_SCRATCH_SIZE];
#if UZESID_ENABLE_CAPTURE
#ifndef UZESID_CAPTURE_MAX_EVENTS
#define UZESID_CAPTURE_MAX_EVENTS ((UZESID_UZSD_HEADER_SIZE - 1u) / 2u)
#endif
typedef char UzesidRuntimeScratchFitsCapture[
	(UZESID_RUNTIME_SCRATCH_SIZE >=
	 (UZESID_UZSD_HEADER_SIZE + UZESID_UZSD_INIT_REG_COUNT)) ? 1 : -1];
#define g_uzesid_capture_header (g_uzesid_runtime_scratch)
#define g_uzesid_capture_prev \
	(g_uzesid_runtime_scratch + UZESID_UZSD_HEADER_SIZE)
#if UZESID_CAPTURE_MAX_EVENTS > ((UZESID_UZSD_HEADER_SIZE - 1u) / 2u)
/* Host conversion can afford a much larger ordered-write queue.  The AVR
 * keeps reusing the 49-byte header buffer and therefore pays no extra SRAM. */
static u8 g_uzesid_capture_events[UZESID_CAPTURE_MAX_EVENTS * 2u];
#define UZESID_CAPTURE_EVENTS g_uzesid_capture_events
#else
#define UZESID_CAPTURE_EVENTS g_uzesid_capture_header
#endif
static u16 g_uzesid_capture_count;
static u8 g_uzesid_capture_overflow;
#endif
u8 g_uzesid_psid_loaded;
u8 g_uzesid_psid_error;
u16 g_uzesid_init_adr;
u8 g_uzesid_play_adr_from_irq_vec;
u32 g_uzesid_speed_flags;

static inline u16 read_psid_16(const u8 *p, int ofs){ return (u16)(((u16)p[ofs] << 8) | p[ofs+1]); }
static inline u32 read_psid_32(const u8 *p, int ofs){ return ((u32)p[ofs] << 24) | ((u32)p[ofs+1] << 16) | ((u32)p[ofs+2] << 8) | (u32)p[ofs+3]; }

void UzesidCaptureNoteWrite(u8 reg, u8 val)
{
#if UZESID_ENABLE_CAPTURE
	u16 pos;
	if(!g_uzesid_capture_enabled || reg >= UZESID_UZSD_INIT_REG_COUNT)
		return;
	/* Exact rewrites have no SID-side effect.  Compare against the immediately
	 * preceding value, not the value at the start of the tick, so gate-off /
	 * gate-on and test-bit pulses remain ordered in the stream. */
	if(g_uzesid_capture_prev[reg] == val)
		return;
	g_uzesid_capture_prev[reg] = val;
	if(g_uzesid_capture_count >= UZESID_CAPTURE_MAX_EVENTS){
		g_uzesid_capture_overflow = 1;
		return;
	}
	pos = (u16)(g_uzesid_capture_count * 2u);
	UZESID_CAPTURE_EVENTS[pos] = reg;
	UZESID_CAPTURE_EVENTS[pos + 1u] = val;
	g_uzesid_capture_count++;
#else
	(void)reg;
	(void)val;
#endif
}

#define CPU_SPI_BASE 0UL
/* A 32-byte, two-way set-associative cache keeps the same 512-byte data
 * footprint as the old two-page cache, but avoids transferring an entire
 * 256-byte page for scattered table accesses.  Trace profiling of the bundled
 * songs reduced SPI traffic by about 90 percent. */
#define CPU_CACHE_BLOCK_SHIFT 5u
#define CPU_CACHE_BLOCK_SIZE  (1u << CPU_CACHE_BLOCK_SHIFT)
#define CPU_CACHE_BLOCKS      16u
#define CPU_CACHE_SETS        8u
#define CPU_CACHE_WAY_STRIDE  CPU_CACHE_SETS
#define CPU_BLOCKS_PER_PAGE   (CPU_PAGE_SIZE / CPU_CACHE_BLOCK_SIZE)
#define CPU_PAGE_SIZE         256u
#define CPU_SRAM_PAGE_ZP      0u
#define CPU_SRAM_PAGE_STACK   1u

static u8 zp_ram[256];
static u8 st_ram[256];
/* Shared with database sector I/O after pre-emulation has finished.  The
 * 6502 cache and SD writer are never active at the same time, so this avoids a
 * second 512-byte SRAM allocation. */
u8 g_uzesid_workbuf[CPU_CACHE_BLOCKS * CPU_CACHE_BLOCK_SIZE];
#define cache_data ((u8 (*)[CPU_CACHE_BLOCK_SIZE])g_uzesid_workbuf)
static u16 cache_tag[CPU_CACHE_BLOCKS];
static u16 cache_valid_mask;
static u16 cache_dirty_mask;
static u8 cache_victim_bits;
static u8 cache_last_slot;
/* Untouched C64 pages are synthesized as 0x00 below $E000 and 0x40 above it.
 * A page is physically initialized only when the emulated CPU first writes it. */
static u8 cpu_page_materialized[32];

static inline u8 cpu_page_is_materialized(u8 page)
{
	return (u8)(cpu_page_materialized[page >> 3] & (u8)(1u << (page & 7u)));
}

static inline void cpu_page_mark_materialized(u8 page)
{
	cpu_page_materialized[page >> 3] |= (u8)(1u << (page & 7u));
}

static inline u8 cpu_default_page_value(u8 page)
{
	return (page >= 0xE0u) ? 0x40u : 0x00u;
}

static void cpu_spiram_block_read(u16 block, u8 *dst)
{
	u16 addr = (u16)(block << CPU_CACHE_BLOCK_SHIFT);
	SpiRamReadInto(0, addr, dst, CPU_CACHE_BLOCK_SIZE);
}

static void cpu_spiram_block_write(u16 block, const u8 *src)
{
	u16 addr = (u16)(block << CPU_CACHE_BLOCK_SHIFT);
	SpiRamWriteFrom(0, addr, (void *)src, CPU_CACHE_BLOCK_SIZE);
}

static void cpu_cache_flush_slot(u8 slot)
{
	u16 bit = (u16)1u << slot;
	if((cache_valid_mask & bit) && (cache_dirty_mask & bit)){
		cpu_spiram_block_write(cache_tag[slot], cache_data[slot]);
		cache_dirty_mask &= (u16)~bit;
	}
}

static void cpu_materialize_page(u8 page, u8 *scratch)
{
	u8 i;
	u8 value = cpu_default_page_value(page);
	u16 first_block = (u16)page << (8u - CPU_CACHE_BLOCK_SHIFT);
	memset(scratch, value, CPU_CACHE_BLOCK_SIZE);
	for(i = 0; i < CPU_BLOCKS_PER_PAGE; i++)
		cpu_spiram_block_write((u16)(first_block + i), scratch);
	cpu_page_mark_materialized(page);
}

static void cpu_cache_reset(void)
{
	cache_valid_mask = 0;
	cache_dirty_mask = 0;
	cache_victim_bits = 0;
	cache_last_slot = 0;
}

static u8 *cpu_cache_get_block(u16 adr, u8 for_write)
{
	u16 block = adr >> CPU_CACHE_BLOCK_SHIFT;
	u8 slot = cache_last_slot;
	u16 bit = (u16)1u << slot;
	u8 page;

	/* Sequential opcode and operand reads overwhelmingly stay in the same
	 * 32-byte block.  Check the most recently used slot before calculating and
	 * probing the two-way set. */
	if(!((cache_valid_mask & bit) && cache_tag[slot] == block)){
		u8 set = (u8)(block & (CPU_CACHE_SETS - 1u));
		u8 slot0 = set;
		u8 slot1 = (u8)(set + CPU_CACHE_WAY_STRIDE);
		u16 bit0 = (u16)1u << slot0;
		u16 bit1 = (u16)1u << slot1;

		if((cache_valid_mask & bit0) && cache_tag[slot0] == block){
			slot = slot0;
			bit = bit0;
		}else if((cache_valid_mask & bit1) && cache_tag[slot1] == block){
			slot = slot1;
			bit = bit1;
		}else{
			if(!(cache_valid_mask & bit0)){
				slot = slot0;
				bit = bit0;
			}else if(!(cache_valid_mask & bit1)){
				slot = slot1;
				bit = bit1;
			}else if(cache_victim_bits & (u8)(1u << set)){
				slot = slot1;
				bit = bit1;
				cache_victim_bits &= (u8)~(1u << set);
			}else{
				slot = slot0;
				bit = bit0;
				cache_victim_bits |= (u8)(1u << set);
			}
			cpu_cache_flush_slot(slot);
			page = (u8)(adr >> 8);
			if(for_write && !cpu_page_is_materialized(page))
				cpu_materialize_page(page, cache_data[slot]);
			if(cpu_page_is_materialized(page))
				cpu_spiram_block_read(block, cache_data[slot]);
			else
				memset(cache_data[slot], cpu_default_page_value(page), CPU_CACHE_BLOCK_SIZE);
			cache_tag[slot] = block;
			cache_valid_mask |= bit;
			cache_dirty_mask &= (u16)~bit;
		}
		cache_last_slot = slot;
	}
	if(for_write){
		page = (u8)(adr >> 8);
		if(!cpu_page_is_materialized(page))
			cpu_materialize_page(page, cache_data[slot]);
		cache_dirty_mask |= bit;
	}
	return cache_data[slot];
}

u8 CPUPeekByte(u16 adr)
{
	u8 page = (u8)(adr >> 8);
	if(page == CPU_SRAM_PAGE_ZP) return zp_ram[(u8)adr];
	if(page == CPU_SRAM_PAGE_STACK) return st_ram[(u8)adr];
	return cpu_cache_get_block(adr, 0)[(u8)(adr & (CPU_CACHE_BLOCK_SIZE - 1u))];
}

void CPUPokeByte(u16 adr, u8 byte)
{
	u8 page = (u8)(adr >> 8);
	if(page == CPU_SRAM_PAGE_ZP){ zp_ram[(u8)adr] = byte; return; }
	if(page == CPU_SRAM_PAGE_STACK){ st_ram[(u8)adr] = byte; return; }
	cpu_cache_get_block(adr, 1)[(u8)(adr & (CPU_CACHE_BLOCK_SIZE - 1u))] = byte;
}

void CPUPokeBlock(u16 adr, const u8 *src, u16 len)
{
	while(len--)
		CPUPokeByte(adr++, *src++);
}

static void emu_mem_zero(u8 *dst, u16 len)
{
	while(len--)
		*dst++ = 0;
}

void MemoryInit() { cpu_cache_reset(); MemoryClear(); }
void MemoryExit() { u8 i; for(i = 0; i < CPU_CACHE_BLOCKS; i++) cpu_cache_flush_slot(i); }
void MemoryClear()
{
	emu_mem_zero(zp_ram, sizeof(zp_ram));
	emu_mem_zero(st_ram, sizeof(st_ram));
	emu_mem_zero(cpu_page_materialized, sizeof(cpu_page_materialized));
	cpu_cache_reset();
	zp_ram[1] = 7;
	CPUPokeByte(0x0314, 0x31);
	CPUPokeByte(0x0315, 0xea);
}


#define CPUEMU_PFLAG_N 0x80
#define CPUEMU_PFLAG_V 0x40
#define CPUEMU_PFLAG_B 0x10
#define CPUEMU_PFLAG_D 0x08
#define CPUEMU_PFLAG_I 0x04
#define CPUEMU_PFLAG_Z 0x02
#define CPUEMU_PFLAG_C 0x01

static __attribute__((noinline)) void cpu_do_adc_op(u8 *ra, u8 byte, u8 *n_flag, u8 *z_flag, u8 *pflags)
{
	u8 a = *ra;
	u8 p = *pflags;
	if (p & CPUEMU_PFLAG_D) {
		u16 tmp, tmp2;
		tmp = (u16)(a & 0x0f) + (u16)(byte & 0x0f) + (u16)(p & CPUEMU_PFLAG_C);
		if (tmp > 9u) tmp = (u16)(tmp + 6u);
		tmp2 = (u16)(tmp & 0x0f) + (u16)(a & 0xf0) + (u16)(byte & 0xf0);
		if (tmp > 0x0fu) tmp2 = (u16)(tmp2 + 0x10u);
		*z_flag = (u8)(a + byte + (u8)(p & CPUEMU_PFLAG_C));
		*n_flag = (u8)tmp2;
		if((!( (a ^ byte) & 0x80u)) && (((a ^ (u8)tmp2) & 0x80u) != 0u)) p |= CPUEMU_PFLAG_V; else p &= (u8)~CPUEMU_PFLAG_V;
		if ((tmp2 & 0x1f0u) > 0x90u) tmp2 = (u16)(tmp2 + 0x60u);
		if (tmp2 & 0xf00u) p |= CPUEMU_PFLAG_C; else p &= (u8)~CPUEMU_PFLAG_C;
		*ra = (u8)tmp2;
	} else {
		u16 tmp = (u16)a + (u16)byte + (u16)(p & CPUEMU_PFLAG_C);
		if (tmp > 0xffu) p |= CPUEMU_PFLAG_C; else p &= (u8)~CPUEMU_PFLAG_C;
		if((!( (a ^ byte) & 0x80u)) && (((a ^ (u8)tmp) & 0x80u) != 0u)) p |= CPUEMU_PFLAG_V; else p &= (u8)~CPUEMU_PFLAG_V;
		*ra = (u8)tmp;
		*n_flag = (u8)tmp;
		*z_flag = (u8)tmp;
	}
	*pflags = p;
}

static __attribute__((noinline)) void cpu_do_sbc_op(u8 *ra, u8 byte, u8 *n_flag, u8 *z_flag, u8 *pflags)
{
	u8 a = *ra;
	u8 p = *pflags;
	u16 tmp = (u16)a - (u16)byte - ((p & CPUEMU_PFLAG_C) ? 0u : 1u);
	if((((a ^ (u8)tmp) & 0x80u) != 0u) && (((a ^ byte) & 0x80u) != 0u)) p |= CPUEMU_PFLAG_V; else p &= (u8)~CPUEMU_PFLAG_V;
	if (p & CPUEMU_PFLAG_D) {
		u16 tmp2 = (u16)(a & 0x0f) - (u16)(byte & 0x0f) - ((p & CPUEMU_PFLAG_C) ? 0u : 1u);
		if (tmp2 & 0x10u)
			tmp2 = (u16)(((tmp2 - 6u) & 0x0fu) | ((u16)(a & 0xf0) - (u16)(byte & 0xf0) - 0x10u));
		else
			tmp2 = (u16)((tmp2 & 0x0fu) | ((u16)(a & 0xf0) - (u16)(byte & 0xf0)));
		if (tmp2 & 0x100u) tmp2 = (u16)(tmp2 - 0x60u);
		*ra = (u8)tmp2;
	} else {
		*ra = (u8)tmp;
	}
	if (tmp < 0x100u) p |= CPUEMU_PFLAG_C; else p &= (u8)~CPUEMU_PFLAG_C;
	*n_flag = (u8)tmp;
	*z_flag = (u8)tmp;
	*pflags = p;
}

// Microsecond-resolution timing functions
extern u64 GetTicks_usec();
extern void Delay_usec(u32 usec);


// Memory access function prototypes
static u8 ram_read(u16 adr);
static void ram_write(u16 adr, u8 byte);
static void cia_write(u16 adr, u8 byte);


/*
 *  Init CPU emulation
 */
void CPUInit()
{
}



/*
 *  Exit CPU emulation
 */

void CPUExit()
{
}


/*
 *  Memory access functions
 */

static inline u8 ram_read(u16 adr)
{
	return CPUPeekByte(adr);
}

static inline void ram_write(u16 adr, u8 byte)
{
	CPUPokeByte(adr, byte);
}

static inline void cia_write(u16 adr, u8 byte)
{
	if(adr == 0xdc04u)
		cia_tl_write(byte);
	else if(adr == 0xdc05u)
		cia_th_write(byte);
	else
		CPUPokeByte(adr, byte);
}

static inline u8 cpu_mem_read(u16 adr, cycle_t now)
{
	if((adr & 0xfc00u) == 0xd400u)
		return (u8)sid_read(adr, now);
	return ram_read(adr);
}

static inline void cpu_mem_write(u16 adr, u8 byte, cycle_t now, u8 rmw)
{
	if((adr & 0xfc00u) == 0xd400u){
		sid_write(adr, byte, now, rmw);
		return;
	}
	if((adr & 0xff00u) == 0xdc00u){
		cia_write(adr, byte);
		return;
	}
	ram_write(adr, byte);
}

/*
 *  CPU emulation loop
 */

void CPUExecute(u16 startadr, u8 init_ra, u8 init_rx, u8 init_ry, cycle_t max_cycles)
{
    // 6510 registers
    u8 a = init_ra, x = init_rx, y = init_ry;
    u8 n_flag = 0, z_flag = 0;
    u8 sp = 0xff, pflags = 0;

    // Program counter
    register u16 pc;

    // Temporary address storage
    u16 adr;

    /* Register-write capture does not model VIC/CIA/SID timing; every mapped
     * handler currently ignores the cycle timestamp.  Counting every 6510
     * cycle with a 32-bit AVR increment was therefore pure overhead.  Keep a
     * cheap instruction watchdog instead so malformed tunes still terminate. */
    const cycle_t current_cycle = 0;
    u16 instruction_blocks = (u16)((max_cycles + 255u) >> 8);
    u8 instruction_low = 0;

#define RA a
#define RX x
#define RY y
#define RSP sp
#define RPC pc
#define N_FLAG n_flag
#define Z_FLAG z_flag
#define PFLAGS pflags
#define ADR adr

#define read_byte(adr)     cpu_mem_read(adr, current_cycle)
#define read_zp(adr) \
    zp_ram[(u8)(adr)]

#define write_byte(adr, byte)     cpu_mem_write(adr, byte, current_cycle, 0)
#define write_byte_rmw(adr, byte)     cpu_mem_write(adr, byte, current_cycle, 1)
#define write_zp(adr, byte) \
    zp_ram[(u8)(adr)] = (byte)

#define read_idle(adr)
#define read_idle_zp(adr)
#define read_idle_stack(sp)

#define read_opcode \
    read_byte(pc)
#define read_idle_opcode

#define push_byte(byte) \
{ \
    if (sp == 0) \
        quit = 1; \
    st_ram[sp--] = (byte); \
}
#define pop_byte \
    (sp == 0xff ? (quit = 1, 0) : st_ram[++sp])

#define jump(adr) \
    pc = (u16)(adr)
#define inc_pc \
    pc++

#define next_cycle do { } while(0)




/*
 *  Status register flag bits
 */

#define PFLAG_N 0x80
#define PFLAG_V 0x40
#define PFLAG_B 0x10
#define PFLAG_D 0x08
#define PFLAG_I 0x04
#define PFLAG_Z 0x02
#define PFLAG_C 0x01


/*
 *  Opcode flag bits
 */

#define OPFLAG_IRQ_DISABLED 0x100
#define OPFLAG_IRQ_ENABLED 0x200
#define OPFLAG_INT_DELAYED 0x400


/*
 *  Address calculation macros
 */

// Read zero page operand address
#define read_adr_zero \
    ADR = read_opcode; inc_pc; next_cycle

// Read zero page x-indexed operand address
#define read_adr_zero_x \
    ADR = read_opcode; inc_pc; next_cycle; \
    read_idle_zp(ADR); next_cycle; \
    ADR = (ADR + RX) & 0xff

// Read zero page y-indexed operand address
#define read_adr_zero_y \
    ADR = read_opcode; inc_pc; next_cycle; \
    read_idle_zp(ADR); next_cycle; \
    ADR = (ADR + RY) & 0xff

// Read absolute operand address
#define read_adr_abs \
    ADR = read_opcode; inc_pc; next_cycle; \
    ADR |= read_opcode << 8; inc_pc; next_cycle

// Read absolute x-indexed operand address (no extra cycle for page crossing)
#define read_adr_abs_x \
{ \
    ADR = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp = ADR + RX; \
    ADR = tmp + (read_opcode << 8); inc_pc; next_cycle; \
    if (tmp >= 0x100) { \
        read_idle((ADR - 0x100) & 0xffff); \
    } else { \
        read_idle(ADR); \
    } \
    next_cycle; \
}

// Read absolute y-indexed operand address (no extra cycle for page crossing)
#define read_adr_abs_y \
{ \
    ADR = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp = ADR + RY; \
    ADR = tmp + (read_opcode << 8); inc_pc; next_cycle; \
    if (tmp >= 0x100) { \
        read_idle((ADR - 0x100) & 0xffff); \
    } else { \
        read_idle(ADR); \
    } \
    next_cycle; \
}

// Read indexed indirect operand address (no extra cycle for page crossing)
#define read_adr_ind_x \
{ \
    unsigned int tmp = read_opcode; inc_pc; next_cycle; \
    read_idle_zp(tmp); next_cycle; \
    tmp += RX; \
    ADR = read_zp(tmp & 0xff); next_cycle; \
    ADR |= read_zp((tmp + 1) & 0xff) << 8; next_cycle; \
}

// Read indirect indexed operand address (no extra cycle for page crossing)
#define read_adr_ind_y \
{ \
    unsigned int tmp = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp2 = read_zp(tmp) + RY; next_cycle; \
    ADR = tmp2 + (read_zp((tmp + 1) & 0xff) << 8); next_cycle; \
    if (tmp2 >= 0x100) { \
        read_idle((ADR - 0x100) & 0xffff); \
    } else { \
        read_idle(ADR); \
    } \
    next_cycle; \
}


/*
 *  Operand fetch macros
 */

// Read immediate operand
#define read_byte_imm(to) \
    to = read_opcode; inc_pc; next_cycle

// Read zero page operand
#define read_byte_zero(to) \
    read_adr_zero; \
    to = read_zp(ADR); next_cycle

// Read zero page x-indexed operand
#define read_byte_zero_x(to) \
    read_adr_zero_x; \
    to = read_zp(ADR); next_cycle

// Read zero page y-indexed operand
#define read_byte_zero_y(to) \
    read_adr_zero_y; \
    to = read_zp(ADR); next_cycle

// Read absolute operand
#define read_byte_abs(to) \
    read_adr_abs; \
    to = read_byte(ADR); next_cycle;

// Read absolute x-indexed operand (extra cycle for page crossing)
#define read_byte_abs_x(to) \
{ \
    ADR = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp = ADR + RX; \
    ADR = tmp + (read_opcode << 8); inc_pc; next_cycle; \
    if (tmp >= 0x100) { \
        /* Page crossed */ \
        read_idle((ADR - 0x100) & 0xffff); next_cycle; \
    } \
    to = read_byte(ADR); next_cycle; \
}

// Read absolute y-indexed operand (extra cycle for page crossing)
#define read_byte_abs_y(to) \
{ \
    ADR = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp = ADR + RY; \
    ADR = tmp + (read_opcode << 8); inc_pc; next_cycle; \
    if (tmp >= 0x100) { \
        /* Page crossed */ \
        read_idle((ADR - 0x100) & 0xffff); next_cycle; \
    } \
    to = read_byte(ADR); next_cycle; \
}

// Read indexed indirect operand (no extra cycle for page crossing)
#define read_byte_ind_x(to) \
    read_adr_ind_x; \
    to = read_byte(ADR); next_cycle;

// Read indirect indexed operand (extra cycle for page crossing)
#define read_byte_ind_y(to) \
{ \
    unsigned int tmp = read_opcode; inc_pc; next_cycle; \
    unsigned int tmp2 = read_zp(tmp) + RY; next_cycle; \
    ADR = tmp2 + (read_zp((tmp + 1) & 0xff) << 8); next_cycle; \
    if (tmp2 >= 0x100) { \
        /* Page crossed */ \
        read_idle((ADR - 0x100) & 0xffff); next_cycle; \
    } \
    to = read_byte(ADR); next_cycle; \
}


/*
 *  Other macros
 */

// Set N and Z flags
#define set_nz(val) \
    N_FLAG = Z_FLAG = val

// Pop status flags
#define pop_flags \
    N_FLAG = PFLAGS = pop_byte & 0xcf; next_cycle; \
    Z_FLAG = !(PFLAGS & PFLAG_Z)

// Push status flags
#ifdef DRIVE_CPU
#define push_flags(b_flag) \
{ \
    gcr_drive->Update(current_cycle); \
    if (gcr_drive->ByteReady()) \
        PFLAGS |= PFLAG_V; \
    (N_FLAG & 0x80) ? (PFLAGS |= PFLAG_N) : (PFLAGS &= ~PFLAG_N); \
    Z_FLAG ? (PFLAGS &= ~PFLAG_Z) : (PFLAGS |= PFLAG_Z); \
    push_byte(PFLAGS | 0x20 | b_flag); next_cycle; \
}
#else
#define push_flags(b_flag) \
    (N_FLAG & 0x80) ? (PFLAGS |= PFLAG_N) : (PFLAGS &= ~PFLAG_N); \
    Z_FLAG ? (PFLAGS &= ~PFLAG_Z) : (PFLAGS |= PFLAG_Z); \
    push_byte(PFLAGS | 0x20 | b_flag); next_cycle
#endif

// ADC operation
#define do_adc(byte) \
    cpu_do_adc_op(&RA, (u8)(byte), &N_FLAG, &Z_FLAG, &PFLAGS); \
    break

// SBC operation
#define do_sbc(byte) \
{ \
    cpu_do_sbc_op(&RA, (u8)(byte), &N_FLAG, &Z_FLAG, &PFLAGS); \
    break; \
}

// CMP operation
#define do_cmp \
    unsigned int tmp = RA - t; \
    set_nz(tmp); \
    (tmp < 0x100) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// CPX operation
#define do_cpx \
    unsigned int tmp = RX - t; \
    set_nz(tmp); \
    (tmp < 0x100) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// CPY operation
#define do_cpy \
    unsigned int tmp = RY - t; \
    set_nz(tmp); \
    (tmp < 0x100) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// BIT operation
#define do_bit \
    Z_FLAG = RA & t; \
    N_FLAG = t; \
    (t & 0x40) ? (PFLAGS |= PFLAG_V) : (PFLAGS &= ~PFLAG_V); \
    break

// ASL operation
#define do_asl(write_cmd) \
    (t & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    next_cycle; write_cmd(ADR, set_nz(t << 1)); next_cycle; \
    break

// LSR operation
#define do_lsr(write_cmd) \
    (t & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    next_cycle; write_cmd(ADR, set_nz(t >> 1)); next_cycle; \
    break

// ROL operation
#define do_rol(write_cmd) \
    next_cycle; write_cmd(ADR, set_nz((t << 1) | (PFLAGS & PFLAG_C))); next_cycle; \
    (t & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// ROR operation
#define do_ror(write_cmd) \
    next_cycle; write_cmd(ADR, set_nz((PFLAGS & PFLAG_C) ? (t >> 1) | 0x80 : (t >> 1))); next_cycle; \
    (t & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// Branch operation
#define branch(flag) \
{ \
    s8 tmp = read_opcode; inc_pc; next_cycle; \
    if (flag) { \
        /* Branch taken */ \
        ADR = RPC + (s8)tmp; \
        if ((ADR ^ RPC) & 0xff00) { \
            /* Page crossed */ \
            read_idle_opcode; next_cycle; \
            if (tmp & 0x80) { \
                read_idle(ADR + 0x100); next_cycle; \
            } else { \
                read_idle(ADR - 0x100); next_cycle; \
            } \
            jump(ADR); \
        } else { \
            /* No page crossed */ \
            opcode |= OPFLAG_INT_DELAYED; \
            read_idle_opcode; next_cycle; \
            jump(ADR); \
        } \
    } \
    break; \
}

// SLO operation
#define do_slo(write_cmd) \
    (t & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    next_cycle; write_cmd(ADR, t <<= 1); next_cycle; \
    set_nz(RA |= t); \
    break

// RLA operation
#define do_rla(write_cmd) \
    unsigned int t2 = (t << 1) | (PFLAGS & PFLAG_C); \
    next_cycle; write_cmd(ADR, t2); next_cycle; \
    (t & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    set_nz(RA &= t2); \
    break

// SRE operation
#define do_sre(write_cmd) \
    (t & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    next_cycle; write_cmd(ADR, t >>= 1); next_cycle; \
    set_nz(RA ^= t); \
    break

// RRA operation
#define do_rra(write_cmd) \
    unsigned int t2 = (PFLAGS & PFLAG_C) ? (t >> 1) | 0x80 : (t >> 1); \
    next_cycle; write_cmd(ADR, t2); next_cycle; \
    (t & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    do_adc(t2);

// DCP operation
#define do_dcp(write_cmd) \
    t = (t - 1) & 0xff; \
    next_cycle; write_cmd(ADR, t); next_cycle; \
    t = RA - t; \
    set_nz(t); \
    (t < 0x100) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C); \
    break

// ISB operation
#define do_isb(write_cmd) \
    t = (t + 1) & 0xff; \
    next_cycle; write_cmd(ADR, t); next_cycle; \
    do_sbc(t);

    // Jump to specified start address
    jump(startadr);

    // Main loop: execute opcodes until stack under-/overflow, RTI, illegal opcode, or max_cycles reached
    u8 quit = 0;
    while (!quit) {
        if(++instruction_low == 0u){
            if(instruction_blocks == 0u || --instruction_blocks == 0u)
                break;
        }

        // Fetch opcode
        u8 opcode = read_opcode; inc_pc; next_cycle;

        // Execute opcode
        switch (opcode) {
/*
 *  Routines for documented opcodes
 */

// Load group
case 0xa9:    // LDA #imm
    read_byte_imm(RA);
    set_nz(RA);
    break;

case 0xa5:    // LDA zero
    read_byte_zero(RA);
    set_nz(RA);
    break;

case 0xb5:    // LDA zero,X
    read_byte_zero_x(RA);
    set_nz(RA);
    break;

case 0xad:    // LDA abs
    read_byte_abs(RA);
    set_nz(RA);
    break;

case 0xbd:    // LDA abs,X
    read_byte_abs_x(RA);
    set_nz(RA);
    break;

case 0xb9:    // LDA abs,Y
    read_byte_abs_y(RA);
    set_nz(RA);
    break;

case 0xa1:    // LDA (ind,X)
    read_byte_ind_x(RA);
    set_nz(RA);
    break;

case 0xb1:    // LDA (ind),Y
    read_byte_ind_y(RA);
    set_nz(RA);
    break;

case 0xa2:    // LDX #imm
    read_byte_imm(RX);
    set_nz(RX);
    break;

case 0xa6:    // LDX zero
    read_byte_zero(RX);
    set_nz(RX);
    break;

case 0xb6:    // LDX zero,Y
    read_byte_zero_y(RX);
    set_nz(RX);
    break;

case 0xae:    // LDX abs
    read_byte_abs(RX);
    set_nz(RX);
    break;

case 0xbe:    // LDX abs,Y
    read_byte_abs_y(RX);
    set_nz(RX);
    break;

case 0xa0:    // LDY #imm
    read_byte_imm(RY);
    set_nz(RY);
    break;

case 0xa4:    // LDY zero
    read_byte_zero(RY);
    set_nz(RY);
    break;

case 0xb4:    // LDY zero,X
    read_byte_zero_x(RY);
    set_nz(RY);
    break;

case 0xac:    // LDY abs
    read_byte_abs(RY);
    set_nz(RY);
    break;

case 0xbc:    // LDY abs,X
    read_byte_abs_x(RY);
    set_nz(RY);
    break;


// Store group
case 0x85:    // STA zero
    read_adr_zero;
    write_zp(ADR, RA); next_cycle;
    break;

case 0x95:    // STA zero,X
    read_adr_zero_x;
    write_zp(ADR, RA); next_cycle;
    break;

case 0x8d:    // STA abs
    read_adr_abs;
    write_byte(ADR, RA); next_cycle;
    break;

case 0x9d:    // STA abs,X
    read_adr_abs_x;
    write_byte(ADR, RA); next_cycle;
    break;

case 0x99:    // STA abs,Y
    read_adr_abs_y;
    write_byte(ADR, RA); next_cycle;
    break;

case 0x81:    // STA (ind,X)
    read_adr_ind_x;
    write_byte(ADR, RA); next_cycle;
    break;

case 0x91:    // STA (ind),Y
    read_adr_ind_y;
    write_byte(ADR, RA); next_cycle;
    break;

case 0x86:    // STX zero
    read_adr_zero;
    write_zp(ADR, RX); next_cycle;
    break;

case 0x96:    // STX zero,Y
    read_adr_zero_y;
    write_zp(ADR, RX); next_cycle;
    break;

case 0x8e:    // STX abs
    read_adr_abs;
    write_byte(ADR, RX); next_cycle;
    break;

case 0x84:    // STY zero
    read_adr_zero;
    write_zp(ADR, RY); next_cycle;
    break;

case 0x94:    // STY zero,X
    read_adr_zero_x;
    write_zp(ADR, RY); next_cycle;
    break;

case 0x8c:    // STY abs
    read_adr_abs;
    write_byte(ADR, RY); next_cycle;
    break;


// Transfer group
case 0xaa:    // TAX
    set_nz(RX = RA);
    read_idle_opcode; next_cycle;
    break;

case 0x8a:    // TXA
    set_nz(RA = RX);
    read_idle_opcode; next_cycle;
    break;

case 0xa8:    // TAY
    set_nz(RY = RA);
    read_idle_opcode; next_cycle;
    break;

case 0x98:    // TYA
    set_nz(RA = RY);
    read_idle_opcode; next_cycle;
    break;

case 0xba:    // TSX
    set_nz(RX = RSP);
    read_idle_opcode; next_cycle;
    break;

case 0x9a:    // TXS
    RSP = RX;
    read_idle_opcode; next_cycle;
    break;


// Arithmetic group
case 0x69: {// ADC #imm
    u8 t;
    read_byte_imm(t);
    do_adc(t);
}
case 0x65: {// ADC zero
    u8 t;
    read_byte_zero(t);
    do_adc(t);
}
case 0x75: {// ADC zero,X
    u8 t;
    read_byte_zero_x(t);
    do_adc(t);
}
case 0x6d: {// ADC abs
    u8 t;
    read_byte_abs(t);
    do_adc(t);
}
case 0x7d: {// ADC abs,X
    u8 t;
    read_byte_abs_x(t);
    do_adc(t);
}
case 0x79: {// ADC abs,Y
    u8 t;
    read_byte_abs_y(t);
    do_adc(t);
}
case 0x61: {// ADC (ind,X)
    u8 t;
    read_byte_ind_x(t);
    do_adc(t);
}
case 0x71: {// ADC (ind),Y
    u8 t;
    read_byte_ind_y(t);
    do_adc(t);
}
case 0xe9:    // SBC #imm
#if UZESID_ENABLE_ILLEGAL_OPS
case 0xeb:    // Undocumented opcode
#endif
{
    u8 t;
    read_byte_imm(t);
    do_sbc(t);
}
case 0xe5: {// SBC zero
    u8 t;
    read_byte_zero(t);
    do_sbc(t);
}
case 0xf5: {// SBC zero,X
    u8 t;
    read_byte_zero_x(t);
    do_sbc(t);
}
case 0xed: {// SBC abs
    u8 t;
    read_byte_abs(t);
    do_sbc(t);
}
case 0xfd: {// SBC abs,X
    u8 t;
    read_byte_abs_x(t);
    do_sbc(t);
}
case 0xf9: {// SBC abs,Y
    u8 t;
    read_byte_abs_y(t);
    do_sbc(t);
}
case 0xe1: {// SBC (ind,X)
    u8 t;
    read_byte_ind_x(t);
    do_sbc(t);
}
case 0xf1: {// SBC (ind),Y
    u8 t;
    read_byte_ind_y(t);
    do_sbc(t);
}


// Increment/decrement group
case 0xe8:    // INX
    set_nz(++RX);
    read_idle_opcode; next_cycle;
    break;

case 0xca:    // DEX
    set_nz(--RX);
    read_idle_opcode; next_cycle;
    break;

case 0xc8:    // INY
    set_nz(++RY);
    read_idle_opcode; next_cycle;
    break;

case 0x88:    // DEY
    set_nz(--RY);
    read_idle_opcode; next_cycle;
    break;

case 0xe6: {// INC zero
    unsigned int t;
    read_byte_zero(t);
    next_cycle; write_zp(ADR, set_nz(t + 1)); next_cycle;
    break;
}
case 0xf6: {// INC zero,X
    unsigned int t;
    read_byte_zero_x(t);
    next_cycle; write_zp(ADR, set_nz(t + 1)); next_cycle;
    break;
}
case 0xee: {// INC abs
    unsigned int t;
    read_byte_abs(t);
    next_cycle; write_byte_rmw(ADR, set_nz(t + 1)); next_cycle;
    break;
}
case 0xfe: {// INC abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    next_cycle; write_byte_rmw(ADR, set_nz(t + 1)); next_cycle;
    break;
}
case 0xc6: {// DEC zero
    unsigned int t;
    read_byte_zero(t);
    next_cycle; write_zp(ADR, set_nz(t - 1)); next_cycle;
    break;
}
case 0xd6: {// DEC zero,X
    unsigned int t;
    read_byte_zero_x(t);
    next_cycle; write_zp(ADR, set_nz(t - 1)); next_cycle;
    break;
}
case 0xce: {// DEC abs
    unsigned int t;
    read_byte_abs(t);
    next_cycle; write_byte_rmw(ADR, set_nz(t - 1)); next_cycle;
    break;
}
case 0xde: {// DEC abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    next_cycle; write_byte_rmw(ADR, set_nz(t - 1)); next_cycle;
    break;
}


// Logic group
case 0x29: {// AND #imm
    u8 t;
    read_byte_imm(t);
    set_nz(RA &= t);
    break;
}
case 0x25: {// AND zero
    u8 t;
    read_byte_zero(t);
    set_nz(RA &= t);
    break;
}
case 0x35: {// AND zero,X
    u8 t;
    read_byte_zero_x(t);
    set_nz(RA &= t);
    break;
}
case 0x2d: {// AND abs
    u8 t;
    read_byte_abs(t);
    set_nz(RA &= t);
    break;
}
case 0x3d: {// AND abs,X
    u8 t;
    read_byte_abs_x(t);
    set_nz(RA &= t);
    break;
}
case 0x39: {// AND abs,Y
    u8 t;
    read_byte_abs_y(t);
    set_nz(RA &= t);
    break;
}
case 0x21: {// AND (ind,X)
    u8 t;
    read_byte_ind_x(t);
    set_nz(RA &= t);
    break;
}
case 0x31: {// AND (ind),Y
    u8 t;
    read_byte_ind_y(t);
    set_nz(RA &= t);
    break;
}
case 0x09: {// ORA #imm
    u8 t;
    read_byte_imm(t);
    set_nz(RA |= t);
    break;
}
case 0x05: {// ORA zero
    u8 t;
    read_byte_zero(t);
    set_nz(RA |= t);
    break;
}
case 0x15: {// ORA zero,X
    u8 t;
    read_byte_zero_x(t);
    set_nz(RA |= t);
    break;
}
case 0x0d: {// ORA abs
    u8 t;
    read_byte_abs(t);
    set_nz(RA |= t);
    break;
}
case 0x1d: {// ORA abs,X
    u8 t;
    read_byte_abs_x(t);
    set_nz(RA |= t);
    break;
}
case 0x19: {// ORA abs,Y
    u8 t;
    read_byte_abs_y(t);
    set_nz(RA |= t);
    break;
}
case 0x01: {// ORA (ind,X)
    u8 t;
    read_byte_ind_x(t);
    set_nz(RA |= t);
    break;
}
case 0x11: {// ORA (ind),Y
    u8 t;
    read_byte_ind_y(t);
    set_nz(RA |= t);
    break;
}
case 0x49: {// EOR #imm
    u8 t;
    read_byte_imm(t);
    set_nz(RA ^= t);
    break;
}
case 0x45: {// EOR zero
    u8 t;
    read_byte_zero(t);
    set_nz(RA ^= t);
    break;
}
case 0x55: {// EOR zero,X
    u8 t;
    read_byte_zero_x(t);
    set_nz(RA ^= t);
    break;
}
case 0x4d: {// EOR abs
    u8 t;
    read_byte_abs(t);
    set_nz(RA ^= t);
    break;
}
case 0x5d: {// EOR abs,X
    u8 t;
    read_byte_abs_x(t);
    set_nz(RA ^= t);
    break;
}
case 0x59: {// EOR abs,Y
    u8 t;
    read_byte_abs_y(t);
    set_nz(RA ^= t);
    break;
}
case 0x41: {// EOR (ind,X)
    u8 t;
    read_byte_ind_x(t);
    set_nz(RA ^= t);
    break;
}
case 0x51: {// EOR (ind),Y
    u8 t;
    read_byte_ind_y(t);
    set_nz(RA ^= t);
    break;
}


// Compare group
case 0xc9: {// CMP #imm
    u8 t;
    read_byte_imm(t);
    do_cmp;
}
case 0xc5: {// CMP zero
    u8 t;
    read_byte_zero(t);
    do_cmp;
}
case 0xd5: {// CMP zero,X
    u8 t;
    read_byte_zero_x(t);
    do_cmp;
}
case 0xcd: {// CMP abs
    u8 t;
    read_byte_abs(t);
    do_cmp;
}
case 0xdd: {// CMP abs,X
    u8 t;
    read_byte_abs_x(t);
    do_cmp;
}
case 0xd9: {// CMP abs,Y
    u8 t;
    read_byte_abs_y(t);
    do_cmp;
}
case 0xc1: {// CMP (ind,X)
    u8 t;
    read_byte_ind_x(t);
    do_cmp;
}
case 0xd1: {// CMP (ind),Y
    u8 t;
    read_byte_ind_y(t);
    do_cmp;
}
case 0xe0: {// CPX #imm
    u8 t;
    read_byte_imm(t);
    do_cpx;
}
case 0xe4: {// CPX zero
    u8 t;
    read_byte_zero(t);
    do_cpx;
}
case 0xec: {// CPX abs
    u8 t;
    read_byte_abs(t);
    do_cpx;
}
case 0xc0: {// CPY #imm
    u8 t;
    read_byte_imm(t);
    do_cpy;
}
case 0xc4: {// CPY zero
    u8 t;
    read_byte_zero(t);
    do_cpy;
}
case 0xcc: {// CPY abs
    u8 t;
    read_byte_abs(t);
    do_cpy;
}


// Bit-test group
case 0x24: {// BIT zero
    u8 t;
    read_byte_zero(t);
    do_bit;
}
case 0x2c: {// BIT abs
    u8 t;
    read_byte_abs(t);
    do_bit;
}


// Shift/rotate group
case 0x0a:    // ASL A
    (RA & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    set_nz(RA <<= 1);
    read_idle_opcode; next_cycle;
    break;

case 0x06: {// ASL zero
    unsigned int t;
    read_byte_zero(t);
    do_asl(write_zp);
}
case 0x16: {// ASL zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_asl(write_zp);
}
case 0x0e: {// ASL abs
    unsigned int t;
    read_byte_abs(t);
    do_asl(write_byte_rmw);
}
case 0x1e: {// ASL abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_asl(write_byte_rmw);
}
case 0x4a:    // LSR A
    (RA & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    set_nz(RA >>= 1);
    read_idle_opcode; next_cycle;
    break;

case 0x46: {// LSR zero
    unsigned int t;
    read_byte_zero(t);
    do_lsr(write_zp);
}
case 0x56: {// LSR zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_lsr(write_zp);
}
case 0x4e: {// LSR abs
    unsigned int t;
    read_byte_abs(t);
    do_lsr(write_byte_rmw);
}
case 0x5e: {// LSR abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_lsr(write_byte_rmw);
}
case 0x2a: {// ROL A
    u8 t = RA;
    set_nz(RA = (RA << 1) | (PFLAGS & PFLAG_C));
    (t & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    read_idle_opcode; next_cycle;
    break;
}
case 0x26: {// ROL zero
    unsigned int t;
    read_byte_zero(t);
    do_rol(write_zp);
}
case 0x36: {// ROL zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_rol(write_zp);
}
case 0x2e: {// ROL abs
    unsigned int t;
    read_byte_abs(t);
    do_rol(write_byte_rmw);
}
case 0x3e: {// ROL abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rol(write_byte_rmw);
}
case 0x6a: {// ROR A
    u8 t = RA;
    set_nz(RA = ((PFLAGS & PFLAG_C) ? ((RA >> 1) | 0x80) : (RA >> 1)));
    (t & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    read_idle_opcode; next_cycle;
    break;
}
case 0x66: {// ROR zero
    unsigned int t;
    read_byte_zero(t);
    do_ror(write_zp);
}
case 0x76: {// ROR zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_ror(write_zp);
}
case 0x6e:    {// ROR abs
    unsigned int t;
    read_byte_abs(t);
    do_ror(write_byte_rmw);
}
case 0x7e:    {// ROR abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_ror(write_byte_rmw);
}


// Stack group
case 0x48:    // PHA
    read_idle_opcode; next_cycle;
    push_byte(RA); next_cycle;
    break;

case 0x68:    // PLA
    read_idle_opcode; next_cycle;
    read_idle_stack(RSP); next_cycle;
    set_nz(RA = pop_byte); next_cycle;
    break;

case 0x08:    // PHP
    read_idle_opcode; next_cycle;
    push_flags(PFLAG_B);
    break;

case 0x28: {// PLP
    read_idle_opcode; next_cycle;
    read_idle_stack(RSP); next_cycle;
    u8 old_pflags = PFLAGS;
    pop_flags;
    if (!(old_pflags & PFLAG_I) && (PFLAGS & PFLAG_I))
        opcode |= OPFLAG_IRQ_DISABLED;
    else if ((old_pflags & PFLAG_I) && !(PFLAGS & PFLAG_I))
        opcode |= OPFLAG_IRQ_ENABLED;
    break;
}


// Jump/branch group
case 0x4c:    // JMP abs
    read_adr_abs;
    jump(ADR);
    break;

case 0x6c: {// JMP (ind)
    read_adr_abs;
    u8 t = read_byte(ADR); next_cycle;
    jump(t | (read_byte(((ADR + 1) & 0xff) | (ADR & 0xff00)) << 8)); next_cycle;
    break;
}
case 0x20: {// JSR abs
    u8 t = read_opcode; inc_pc; next_cycle;
    read_idle_stack(RSP); next_cycle;
    push_byte(RPC >> 8); next_cycle;
    push_byte(RPC); next_cycle;
    jump(t | (read_opcode << 8)); next_cycle;
    break;
}
case 0x60: {// RTS
    read_idle_opcode; next_cycle;
    read_idle_stack(RSP); next_cycle;
    u8 t = pop_byte; next_cycle;
    jump(t | (pop_byte << 8)); inc_pc; next_cycle;
    read_idle_opcode; next_cycle;
    break;
}
case 0x40: {// RTI
    quit = 1;
    break;
}
case 0x00: {// BRK
    read_idle_opcode; inc_pc; next_cycle;
    push_byte(RPC >> 8); next_cycle;
    push_byte(RPC); next_cycle;
    push_flags(PFLAG_B);
    PFLAGS |= PFLAG_I;
    u8 t = read_byte(0xfffe); next_cycle;
    jump(t | (read_byte(0xffff) << 8 )); next_cycle;
    break;
}
case 0xb0:    // BCS rel
    branch(PFLAGS & PFLAG_C);

case 0x90:    // BCC rel
    branch(!(PFLAGS & PFLAG_C));

case 0xf0:    // BEQ rel
    branch(!Z_FLAG);

case 0xd0:    // BNE rel
    branch(Z_FLAG);

case 0x70:    // BVS rel
#ifdef DRIVE_CPU
    gcr_drive->Update(current_cycle);
    if (gcr_drive->ByteReady())
        branch(1)
    else
#endif
    branch(PFLAGS & PFLAG_V);

case 0x50:    // BVC rel
#ifdef DRIVE_CPU
    gcr_drive->Update(current_cycle);
    if (gcr_drive->ByteReady())
        branch(0)
    else
#endif
    branch(!(PFLAGS & PFLAG_V));

case 0x30:    // BMI rel
    branch(N_FLAG & 0x80);

case 0x10:    // BPL rel
    branch(!(N_FLAG & 0x80));


// Flags group
case 0x38:    // SEC
    PFLAGS |= PFLAG_C;
    read_idle_opcode; next_cycle;
    break;

case 0x18:    // CLC
    PFLAGS &= ~PFLAG_C;
    read_idle_opcode; next_cycle;
    break;

case 0xf8:    // SED
    PFLAGS |= PFLAG_D;
    read_idle_opcode; next_cycle;
    break;

case 0xd8:    // CLD
    PFLAGS &= ~PFLAG_D;
    read_idle_opcode; next_cycle;
    break;

case 0x78:    // SEI
    if (!(PFLAGS & PFLAG_I))
        opcode |= OPFLAG_IRQ_DISABLED;
    PFLAGS |= PFLAG_I;
    read_idle_opcode; next_cycle;
    break;

case 0x58:    // CLI
    if (PFLAGS & PFLAG_I)
        opcode |= OPFLAG_IRQ_ENABLED;
    PFLAGS &= ~PFLAG_I;
    read_idle_opcode; next_cycle;
    break;

case 0xb8:    // CLV
    PFLAGS &= ~PFLAG_V;
    read_idle_opcode; next_cycle;
    break;


// NOP group
case 0xea:    // NOP
    read_idle_opcode; next_cycle;
    break;


#if UZESID_ENABLE_ILLEGAL_OPS
/*
 *  Routines for undocumented opcodes
 */

// Load A/X group
case 0xa7:    // LAX zero
    read_byte_zero(RA);
    set_nz(RX = RA);
    break;

case 0xb7:    // LAX zero,Y
    read_byte_zero_y(RA);
    set_nz(RX = RA);
    break;

case 0xaf:    // LAX abs
    read_byte_abs(RA);
    set_nz(RX = RA);
    break;

case 0xbf:    // LAX abs,Y
    read_byte_abs_y(RA);
    set_nz(RX = RA);
    break;

case 0xa3:    // LAX (ind,X)
    read_byte_ind_x(RA);
    set_nz(RX = RA);
    break;

case 0xb3:    // LAX (ind),Y
    read_byte_ind_y(RA);
    set_nz(RX = RA);
    break;


// Store A/X group
case 0x87:    // SAX zero
    read_adr_zero;
    write_zp(ADR, RA & RX); next_cycle;
    break;

case 0x97:    // SAX zero,Y
    read_adr_zero_y;
    write_zp(ADR, RA & RX); next_cycle;
    break;

case 0x8f:    // SAX abs
    read_adr_abs;
    write_byte(ADR, RA & RX); next_cycle;
    break;

case 0x83:    // SAX (ind,X)
    read_adr_ind_x;
    write_byte(ADR, RA & RX); next_cycle;
    break;


// ASL/ORA group
case 0x07: {// SLO zero
    unsigned int t;
    read_byte_zero(t);
    do_slo(write_zp);
}
case 0x17: {// SLO zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_slo(write_zp);
}
case 0x0f: {// SLO abs
    unsigned int t;
    read_byte_abs(t);
    do_slo(write_byte_rmw);
}
case 0x1f: {// SLO abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_slo(write_byte_rmw);
}
case 0x1b: {// SLO abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_slo(write_byte_rmw);
}
case 0x03: {// SLO (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_slo(write_byte_rmw);
}
case 0x13: {// SLO (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_slo(write_byte_rmw);
}


// ROL/AND group
case 0x27: {// RLA zero
    unsigned int t;
    read_byte_zero(t);
    do_rla(write_zp);
}
case 0x37: {// RLA zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_rla(write_zp);
}
case 0x2f: {// RLA abs
    unsigned int t;
    read_byte_abs(t);
    do_rla(write_byte_rmw);
}
case 0x3f: {// RLA abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rla(write_byte_rmw);
}
case 0x3b: {// RLA abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rla(write_byte_rmw);
}
case 0x23: {// RLA (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rla(write_byte_rmw);
}
case 0x33: {// RLA (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rla(write_byte_rmw);
}


// LSR/EOR group
case 0x47: {// SRE zero
    unsigned int t;
    read_byte_zero(t);
    do_sre(write_zp);
}
case 0x57: {// SRE zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_sre(write_zp);
}
case 0x4f: {// SRE abs
    unsigned int t;
    read_byte_abs(t);
    do_sre(write_byte_rmw);
}
case 0x5f: {// SRE abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_sre(write_byte_rmw);
}
case 0x5b: {// SRE abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_sre(write_byte_rmw);
}
case 0x43: {// SRE (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_sre(write_byte_rmw);
}
case 0x53: {// SRE (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_sre(write_byte_rmw);
}


// ROR/ADC group
case 0x67: {// RRA zero
    unsigned int t;
    read_byte_zero(t);
    do_rra(write_zp);
}
case 0x77: {// RRA zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_rra(write_zp);
}
case 0x6f: {// RRA abs
    unsigned int t;
    read_byte_abs(t);
    do_rra(write_byte_rmw);
}
case 0x7f: {// RRA abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rra(write_byte_rmw);
}
case 0x7b: {// RRA abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rra(write_byte_rmw);
}
case 0x63: {// RRA (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rra(write_byte_rmw);
}
case 0x73: {// RRA (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_rra(write_byte_rmw);
}


// DEC/CMP group
case 0xc7: {// DCP zero
    unsigned int t;
    read_byte_zero(t);
    do_dcp(write_zp);
}
case 0xd7: {// DCP zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_dcp(write_zp);
}
case 0xcf: {// DCP abs
    unsigned int t;
    read_byte_abs(t);
    do_dcp(write_byte_rmw);
}
case 0xdf: {// DCP abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_dcp(write_byte_rmw);
}
case 0xdb: {// DCP abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_dcp(write_byte_rmw);
}
case 0xc3: {// DCP (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_dcp(write_byte_rmw);
}
case 0xd3: {// DCP (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_dcp(write_byte_rmw);
}


// INC/SBC group
case 0xe7: {// ISB zero
    unsigned int t;
    read_byte_zero(t);
    do_isb(write_zp);
}
case 0xf7: {// ISB zero,X
    unsigned int t;
    read_byte_zero_x(t);
    do_isb(write_zp);
}
case 0xef: {// ISB abs
    unsigned int t;
    read_byte_abs(t);
    do_isb(write_byte_rmw);
}
case 0xff: {// ISB abs,X
    read_adr_abs_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_isb(write_byte_rmw);
}
case 0xfb: {// ISB abs,Y
    read_adr_abs_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_isb(write_byte_rmw);
}
case 0xe3: {// ISB (ind,X)
    read_adr_ind_x;
    unsigned int t = read_byte(ADR); next_cycle;
    do_isb(write_byte_rmw);
}
case 0xf3: {// ISB (ind),Y
    read_adr_ind_y;
    unsigned int t = read_byte(ADR); next_cycle;
    do_isb(write_byte_rmw);
}


// Complex functions
case 0x0b:    // ANC #imm
case 0x2b: {
    u8 t;
    read_byte_imm(t);
    set_nz(RA &= t);
    (N_FLAG & 0x80) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    break;
}
case 0x4b: {// ASR #imm
    u8 t;
    read_byte_imm(t);
    RA &= t;
    (RA & 0x01) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    set_nz(RA >>= 1);
    break;
}
case 0x6b: {// ARR #imm
    unsigned int t;
    read_byte_imm(t);
    t &= RA;
    RA = ((PFLAGS & PFLAG_C) ? (t >> 1) | 0x80 : (t >> 1));
    if (PFLAGS & PFLAG_D) {
        N_FLAG = (PFLAGS & PFLAG_C) << 7;
        Z_FLAG = RA;
        ((t ^ RA) & 0x40) ? (PFLAGS |= PFLAG_V) : (PFLAGS &= ~PFLAG_V);
        if ((t & 0x0f) + (t & 0x01) > 5)
            RA = (RA & 0xf0) | ((RA + 6) & 0x0f);
        (((t + (t & 0x10)) & 0x1f0) > 0x50) ? (PFLAGS |= PFLAG_C, RA += 0x60) : (PFLAGS &= ~PFLAG_C);
    } else {
        set_nz(RA);
        (RA & 0x40) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
        ((RA & 0x40) ^ ((RA & 0x20) << 1)) ? (PFLAGS |= PFLAG_V) : (PFLAGS &= ~PFLAG_V);
    }
    break;
}
case 0x8b: {// ANE #imm
    u8 t;
    read_byte_imm(t);
    set_nz(RA = (RA | 0xee) & RX & t);
    break;
}
case 0x93: {// SHA (ind),Y
    read_adr_ind_y;
    write_byte(ADR, RA & RX & (((ADR - RY) >> 8) + 1)); next_cycle;
    break;
}
case 0x9b: {// SHS abs,Y
    read_adr_abs_y;
    write_byte(ADR, (RSP = RA & RX) & (((ADR - RY) >> 8) + 1)); next_cycle;
    break;
}
case 0x9c: {// SHY abs,X
    read_adr_abs_x;
    write_byte(ADR, RY & (((ADR - RX) >> 8) + 1)); next_cycle;
    break;
}
case 0x9e: {// SHX abs,Y
    read_adr_abs_y;
    write_byte(ADR, RX & (((ADR - RY) >> 8) + 1)); next_cycle;
    break;
}
case 0x9f: {// SHA abs,Y
    read_adr_abs_y;
    write_byte(ADR, RA & RX & (((ADR - RY) >> 8) + 1)); next_cycle;
    break;
}
case 0xab: {// LXA #imm
    u8 t;
    read_byte_imm(t);
    set_nz(RA = RX = (RA | 0xee) & t);
    break;
}
case 0xbb: {// LAS abs,Y
    u8 t;
    read_byte_abs_y(t);
    set_nz(RA = RX = RSP = t & RSP);
    break;
}
case 0xcb: {// SBX #imm
    unsigned int t;
    read_byte_imm(t);
    set_nz(RX = t = (RA & RX) - t);
    (t < 0x100) ? (PFLAGS |= PFLAG_C) : (PFLAGS &= ~PFLAG_C);
    break;
}


// NOP group
case 0x1a:    // NOP
case 0x3a:
case 0x5a:
case 0x7a:
case 0xda:
case 0xfa:
    read_idle_opcode; next_cycle;
    break;

case 0x80:    // NOP #imm
case 0x82:
case 0x89:
case 0xc2:
case 0xe2:
    read_idle_opcode; inc_pc; next_cycle;
    break;

case 0x04:    // NOP zero
case 0x44:
case 0x64:
    read_adr_zero;
    read_idle_zp(ADR); next_cycle;
    break;

case 0x14:    // NOP zero,X
case 0x34:
case 0x54:
case 0x74:
case 0xd4:
case 0xf4:
    read_adr_zero_x;
    read_idle_zp(ADR); next_cycle;
    break;

case 0x0c:    // NOP abs
    read_adr_abs;
    read_idle(ADR); next_cycle;
    break;

case 0x1c:    // NOP abs,X
case 0x3c:
case 0x5c:
case 0x7c:
case 0xdc:
case 0xfc: {
    u8 t; (void)t;//u8 t;
    read_byte_abs_x(t);
    break;
}


#endif

// Jam group
case 0x02:
case 0x12:
case 0x22:
case 0x32:
case 0x42:
case 0x52:
case 0x62:
case 0x72:
case 0x92:
case 0xb2:
case 0xd2:
    goto illegal_op;
            case 0xf2:
            default:
illegal_op:        quit = 1;
                break;
        }
    }
}

static int hex_nibble(char c)
{
	if(c >= '0' && c <= '9') return (int)(c - '0');
	if(c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
	if(c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
	return -1;
}

int UzesidParseMd5Hex(const char *hex, u8 md5[16])
{
	int i;
	if(hex == 0 || md5 == 0) return -1;
	for(i = 0; i < 16; i++)
	{
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);
		if(hi < 0 || lo < 0) return -1;
		md5[i] = (u8)((hi << 4) | lo);
	}
	return (hex[32] == 0) ? 0 : -1;
}

void EmulationUpdatePlayAdr(void)
{
	if(g_uzesid_play_adr_from_irq_vec){
		if(CPUPeekByte(0x0001) & 2)
			play_adr = (u16)(((u16)CPUPeekByte(0x0315) << 8) | CPUPeekByte(0x0314));
		else
			play_adr = (u16)(((u16)CPUPeekByte(0xffff) << 8) | CPUPeekByte(0xfffe));
	}
}

u8 IsPSIDLoaded(void)
{
	return g_uzesid_psid_loaded;
}


void SelectSong(u8 num)
{
	u8 vbi_hz;
	if(number_of_songs <= 0)
		return;
	current_song = (int)(num % (u8)number_of_songs);
	SIDReset(0);
	/* A PSID speed bit selects CIA timing; it does not mean 60 Hz.  Seed the
	 * timer with the machine VBI rate, then let a CIA-timed init routine replace
	 * it through $DC04/$DC05. */
	vbi_hz = (cycles_per_second > 1000000UL) ? 60u : 50u;
	cia_timer = (u16)(cycles_per_second / (u32)vbi_hz - 1u);
	CPUExecute(g_uzesid_init_adr, (u8)current_song, 0, 0, 1000000);
	g_uzesid_psid_loaded = 1;
}

/* -------- minimal MD5 -------- */
typedef struct {
	u32 state[4];
	u32 bit_count;
	u8 buffer[64];
	u32 used;
} md5_ctx_t;

static inline u32 md5_rotl(u32 x, u32 s){ return (x << s) | (x >> (32u - s)); }

static const u32 md5_k[64] PROGMEM = {
	0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
	0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
	0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
	0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
	0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
	0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
	0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
	0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
/* Each MD5 round repeats four rotation counts four times.  Store the sixteen
 * unique round/position values and derive the index from the step number. */
static const u8 md5_s[16] PROGMEM = {
	7,12,17,22, 5,9,14,20, 4,11,16,23, 6,10,15,21
};
#define MD5_S_INDEX(i) ((u8)(((i) >> 2) & 0x0cu) | ((i) & 0x03u))
#ifdef __AVR__
#define MD5_K_AT(i) ((u32)pgm_read_dword(&md5_k[(i)]))
#define MD5_S_AT(i) ((u8)pgm_read_byte(&md5_s[MD5_S_INDEX(i)]))
#else
#define MD5_K_AT(i) (md5_k[(i)])
#define MD5_S_AT(i) (md5_s[MD5_S_INDEX(i)])
#endif

static void md5_transform(md5_ctx_t *ctx, const u8 block[64]){
	u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3], f, g, tmp, x[16];
	u8 i;
	for(i=0;i<16;i++) x[i] = (u32)block[i*4] | ((u32)block[i*4+1] << 8) | ((u32)block[i*4+2] << 16) | ((u32)block[i*4+3] << 24);
	for(i=0;i<64;i++){
		if(i < 16){ f = (b & c) | (~b & d); g = i; }
		else if(i < 32){ f = (d & b) | (~d & c); g = (u8)((5*i + 1) & 15); }
		else if(i < 48){ f = b ^ c ^ d; g = (u8)((3*i + 5) & 15); }
		else { f = c ^ (b | ~d); g = (u8)((7*i) & 15); }
		tmp = d; d = c; c = b;
		b = b + md5_rotl(a + f + MD5_K_AT(i) + x[g], (u32)MD5_S_AT(i));
		a = tmp;
	}
	ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
}

static void md5_init(md5_ctx_t *ctx){ ctx->state[0]=0x67452301u; ctx->state[1]=0xefcdab89u; ctx->state[2]=0x98badcfeu; ctx->state[3]=0x10325476u; ctx->bit_count=0; ctx->used=0; }
static void md5_update(md5_ctx_t *ctx, const void *data_, u16 len){
	const u8 *data = (const u8*)data_;
	while(len--){
		ctx->buffer[ctx->used++] = *data++;
		ctx->bit_count += 8u;
		if(ctx->used == 64u){ md5_transform(ctx, ctx->buffer); ctx->used = 0; }
	}
}
static void md5_final(md5_ctx_t *ctx, u8 out[16]){
	u8 pad = 0x80;
	u8 zero = 0;
	u8 len_le[8];
	u8 i;
	len_le[0] = (u8)(ctx->bit_count & 0xffu);
	len_le[1] = (u8)((ctx->bit_count >> 8) & 0xffu);
	len_le[2] = (u8)((ctx->bit_count >> 16) & 0xffu);
	len_le[3] = (u8)((ctx->bit_count >> 24) & 0xffu);
	len_le[4] = 0;
	len_le[5] = 0;
	len_le[6] = 0;
	len_le[7] = 0;
	md5_update(ctx, &pad, 1);
	while(ctx->used != 56u) md5_update(ctx, &zero, 1);
	md5_update(ctx, len_le, 8);
	for(i = 0; i < 4; i++){
		out[i*4+0] = (u8)(ctx->state[i] & 0xffu);
		out[i*4+1] = (u8)((ctx->state[i] >> 8) & 0xffu);
		out[i*4+2] = (u8)((ctx->state[i] >> 16) & 0xffu);
		out[i*4+3] = (u8)((ctx->state[i] >> 24) & 0xffu);
	}
}

#ifdef UZESID_HAVE_PFF
/* The bundled LIDX is keyed by a whole-file SID MD5, so keep this distinct
 * from the traditional HVSC tune fingerprint algorithm. */
UZESID_NOINLINE static u8 compute_file_md5_pff(const char *path, u8 out[16])
{
	FRESULT res;
	md5_ctx_t md5;
	UINT br;
	md5_init(&md5);
	res = pf_open(path);
	if(res != FR_OK) return 0;
	for(;;){
		if(pf_read(md5.buffer, sizeof(md5.buffer), &br) != FR_OK) return 0;
		while(rcv_spi() != 0xFF);
		if(br == 0) break;
		md5_update(&md5, md5.buffer, (u16)br);
		if(br < sizeof(md5.buffer)) break;
	}
	md5_final(&md5, out);
	return 1;
}

/* The PSID header and the current USDC entry intentionally share the same
 * 124-byte scratch object. Copy each field backward and in descending source
 * order so the overlapping moves cannot destroy a later source. */
static void psid_copy_text_overlap(char *dst, const u8 *src, u8 count)
{
	while(count != 0u){
		count--;
		dst[count] = (char)src[count];
	}
}

#define PSID_PAYLOAD_CHUNK 32u

UZESID_NOINLINE u8 LoadPSIDFilePff(const char *file, u8 md5_out[16], UzesidUsdcEntry *scratch_entry, s16 requested_song)
{
	FRESULT res;
	UINT br;
	u8 *header;
	u8 *buf;
	u16 load_word;
	if(scratch_entry == 0)
		return 0;
	header = (u8 *)scratch_entry;
	buf = (u8 *)scratch_entry;
	u16 load_adr;
	u16 data_offset;
	u32 payload_size;
	u32 ofs;

	g_uzesid_psid_error = UZESID_PSID_ERROR_INVALID;
	res = pf_open(file);
	if(res != FR_OK) return 0;
	memset(header, 0, PSID_MAX_HEADER_LENGTH);
	if(pf_read(header, PSID_MAX_HEADER_LENGTH, &br) != FR_OK) return 0;
	while(rcv_spi() != 0xFF);
	if(br < PSID_MIN_HEADER_LENGTH){
		g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
		return 0;
	}
	if(read_psid_32(header, PSID_ID) != 0x50534944u) return 0;
	if(read_psid_16(header, PSID_VERSION) < 1 || read_psid_16(header, PSID_VERSION) > 2) return 0;

	data_offset = read_psid_16(header, PSID_LENGTH);
	if(data_offset < PSID_MIN_HEADER_LENGTH) return 0;
	if((u32)data_offset > fs.fsize){
		g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
		return 0;
	}

	/* Validate the complete load range before disturbing the currently loaded
	 * song.  A rejected file should leave the prior player state usable. */
	load_adr = read_psid_16(header, PSID_START);
	if(pf_lseek(data_offset) != FR_OK) return 0;
	if(load_adr == 0){
		if((fs.fsize - data_offset) < 2u){
			g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
			return 0;
		}
		if(pf_read(&load_word, 2, &br) != FR_OK || br != 2){
			g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
			return 0;
		}
		while(rcv_spi() != 0xFF);
		load_adr = load_word;
		data_offset += 2;
	}
	payload_size = fs.fsize - data_offset;
	if(payload_size == 0){
		g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
		return 0;
	}
	if(payload_size > (RAM_SIZE - (u32)load_adr)){
		g_uzesid_psid_error = UZESID_PSID_ERROR_TOO_LARGE;
		return 0;
	}

	/* Extract all header metadata before reusing scratch as the 64-byte
	 * payload buffer. */
	g_uzesid_psid_loaded = 0;
	number_of_songs = read_psid_16(header, PSID_NUMBER);
	if(number_of_songs <= 0) number_of_songs = 1;
	current_song = read_psid_16(header, PSID_DEFSONG);
	if(current_song > 0) current_song--;
	if(current_song >= number_of_songs) current_song = 0;
	g_uzesid_init_adr = read_psid_16(header, PSID_INIT);
	play_adr = read_psid_16(header, PSID_MAIN);
	g_uzesid_play_adr_from_irq_vec = (play_adr == 0);
	g_uzesid_speed_flags = read_psid_32(header, PSID_SPEED);
	SIDSetClockHz(SID_DEFAULT_CLOCK_HZ);
	/* PSID v2 clock bits: 01=PAL, 10=NTSC, 00/11=unspecified/both. */
	if(read_psid_16(header, PSID_VERSION) >= 2u && data_offset >= (PSID_FLAGS + 2u)){
		u8 clock_id = (u8)((read_psid_16(header, PSID_FLAGS) >> 2) & 3u);
		if(clock_id == 1u)
			SIDSetClockHz(985248UL);
		else if(clock_id == 2u)
			SIDSetClockHz(1022727UL);
	}
	{
		UzesidUsdcEntry *meta = scratch_entry;
		/* Descending field order is required because source and destination
		 * ranges overlap inside the shared 124-byte scratch object. */
		psid_copy_text_overlap(meta->released, header + PSID_COPYRIGHT, (u8)(sizeof(meta->released) - 1u));
		meta->released[sizeof(meta->released) - 1u] = 0;
		psid_copy_text_overlap(meta->author, header + PSID_AUTHOR, (u8)(sizeof(meta->author) - 1u));
		meta->author[sizeof(meta->author) - 1u] = 0;
		psid_copy_text_overlap(meta->title, header + PSID_NAME, (u8)(sizeof(meta->title) - 1u));
		meta->title[sizeof(meta->title) - 1u] = 0;
	}
	if(g_uzesid_init_adr == 0) g_uzesid_init_adr = load_adr;
	if(requested_song >= 0 && requested_song < number_of_songs)
		current_song = requested_song;

	UzeSidLoadProgress(1);
	MemoryClear();
	ofs = load_adr;
	while(payload_size != 0){
		u16 want = (u16)(payload_size > PSID_PAYLOAD_CHUNK ? PSID_PAYLOAD_CHUNK : payload_size);
		if(pf_read(buf, want, &br) != FR_OK || br != want){
			g_uzesid_psid_error = UZESID_PSID_ERROR_TRUNCATED;
			return 0;
		}
		while(rcv_spi() != 0xFF);
		CPUPokeBlock((u16)ofs, buf, want);
		ofs += want;
		payload_size -= want;
	}
	if(md5_out != 0){
		UzeSidLoadProgress(2);
		if(!compute_file_md5_pff(file, md5_out)) memset(md5_out, 0, 16);
	}
	UzeSidLoadProgress(3);
	SIDReset(0);
	SelectSong((u8)current_song);
	g_uzesid_psid_loaded = 1;
	g_uzesid_psid_error = UZESID_PSID_ERROR_NONE;
	return 1;
}
#else
UZESID_NOINLINE u8 LoadPSIDFilePff(const char *file, u8 md5_out[16], UzesidUsdcEntry *scratch_entry, s16 requested_song) { (void)file; (void)scratch_entry; (void)requested_song; if(md5_out) memset(md5_out,0,16); return 0; }
#endif

static u16 replay_freq_hz(void)
{
	u8 speed_bit = (current_song < 32) ? (u8)current_song : 31u;
	u32 div;
	u32 f;

	/* A clear PSID speed bit is VBI-driven.  A set bit means the play routine
	 * runs from CIA timer A, which may be far above 50/60 Hz (Barbarian uses
	 * roughly 400 Hz). */
	if((g_uzesid_speed_flags & (1ul << speed_bit)) == 0u)
		return (cycles_per_second > 1000000UL) ? 60u : 50u;
	div = (u32)cia_timer + 1u;
	if(div == 0u)
		return 60u;
	f = (cycles_per_second + (div >> 1)) / div;
	if(f < 1u) f = 1u;
	if(f > UZESID_UZSD_TICK_MAX) f = UZESID_UZSD_TICK_MAX;
	return (u16)f;
}

static void emu_ofs_to_bank_addr(u32 ofs, u8 *bank, u16 *addr)
{
	*bank = (u8)(ofs >> 16);
	*addr = (u16)(ofs & 0xffffu);
}

static u8 temp_write(u32 ofs, const void *src_, u16 len)
{
	const u8 *src = (const u8*)src_;
	while(len){
		u8 bank;
		u16 addr;
		u16 chunk;
		emu_ofs_to_bank_addr(ofs, &bank, &addr);
		chunk = (addr == 0u) ? len : (u16)(0x10000UL - addr);
		if(chunk > len) chunk = len;
		SpiRamWriteFrom(bank, addr, (void*)src, chunk);
		ofs += chunk;
		src += chunk;
		len -= chunk;
	}
	return 1;
}


static u8 emit_bytes(u32 *write_ofs, u32 temp_end, const void *src, u16 len)
{
	if(*write_ofs + len > temp_end) return 0;
	temp_write(*write_ofs, src, len);
	*write_ofs += len;
	return 1;
}

static u8 emit_uleb128(u32 *write_ofs, u32 temp_end, u32 value)
{
	u8 b;
	do{
		b = (u8)(value & 0x7fu);
		value >>= 7;
		if(value) b |= 0x80u;
		if(!emit_bytes(write_ofs, temp_end, &b, 1)) return 0;
	}while(value);
	return 1;
}

static u8 emit_tick_skip_run(u32 *write_ofs, u32 temp_end, u32 run)
{
	u8 op = UZESID_UZSD_OP_SKIP;
	if(run == 0u) return 1;
	if(!emit_bytes(write_ofs, temp_end, &op, 1)) return 0;
	return emit_uleb128(write_ofs, temp_end, run);
}

static u8 emit_tick_interleaved(u32 *write_ofs, u32 temp_end, const u8 *events, u16 count)
{
	u8 op = UZESID_UZSD_OP_PAIRS;
	if(!emit_bytes(write_ofs, temp_end, &op, 1)) return 0;
	{
		u8 count8 = (u8)count;
		if(!emit_bytes(write_ofs, temp_end, &count8, 1)) return 0;
	}
	return emit_bytes(write_ofs, temp_end, events, count * 2u);
}

static u8 emit_stream_end(u32 *write_ofs, u32 temp_end)
{
	u8 op = UZESID_UZSD_OP_END;
	return emit_bytes(write_ofs, temp_end, &op, 1);
}

static u8 uleb128_size(u32 value)
{
	u8 n = 1;
	while(value >= 0x80u){
		value >>= 7;
		n++;
	}
	return n;
}

static void capture_wr32(u8 *p, u32 value)
{
	p[0] = (u8)value;
	p[1] = (u8)(value >> 8);
	p[2] = (u8)(value >> 16);
	p[3] = (u8)(value >> 24);
}

UZESID_NOINLINE u8 UzesidCaptureCurrentSongToSpi(u32 temp_ofs, u32 temp_capacity, u32 song_length_ms, u32 *out_total_size, u32 *out_song_length_ms, u16 *out_tick_hz)
{
	u8 *header = g_uzesid_capture_header;
	u8 *prev_regs = g_uzesid_capture_prev;
	u32 header_ofs = temp_ofs;
	u32 write_ofs = temp_ofs + UZESID_UZSD_HEADER_SIZE;
	u32 temp_end = temp_ofs + temp_capacity;
	u32 total_ticks;
	u32 attempted_ticks = 0;
	u32 committed_ticks = 0;
	u32 pending_empty_ticks = 0;
	u16 tick_hz;
	u8 truncated = 0;
	u8 trunc_reason = 0;

	if(!g_uzesid_psid_loaded) return 0;
	if(temp_capacity <= UZESID_UZSD_HEADER_SIZE + 2u) return 0;
	if(song_length_ms == 0u) song_length_ms = 180000u;
	memset(header, 0, UZESID_UZSD_HEADER_SIZE);
	header[0] = 'U'; header[1] = 'Z'; header[2] = 'S'; header[3] = 'D';
	header[4] = UZESID_UZSD_VERSION;
	header[6] = (u8)current_song;
	tick_hz = replay_freq_hz();
	header[5] = (u8)tick_hz;
	header[7] = (u8)((tick_hz >> 8) << UZESID_UZSD_TICK_HI_SHIFT);
	capture_wr32(header + 8, cycles_per_second);
	capture_wr32(header + 12, song_length_ms);
	UzeSID_CopyRegs(header + 24, UZESID_UZSD_INIT_REG_COUNT);
	memcpy(prev_regs, header + 24, UZESID_UZSD_INIT_REG_COUNT);
	total_ticks = (u32)((song_length_ms * (u32)tick_hz + 500u) / 1000u);
	if(total_ticks == 0u) total_ticks = 1u;
	capture_wr32(header + 16, total_ticks);
	if(!temp_write(header_ofs, header, UZESID_UZSD_HEADER_SIZE)) return 0;
	UzeSidCaptureProgress(0, total_ticks);

	while(attempted_ticks < total_ticks){
		u16 event_count;
		u32 event_size;
		u32 required;

		g_uzesid_capture_count = 0;
		g_uzesid_capture_overflow = 0;
		g_uzesid_capture_enabled = 1;
		EmulationUpdatePlayAdr();
		CPUExecute(play_adr, 0, 0, 0, 1000000);
		g_uzesid_capture_enabled = 0;
		attempted_ticks++;
		event_count = g_uzesid_capture_count;

		/* A tick that exceeds the ordered-event scratch limit cannot be encoded
		 * faithfully. Stop with a valid truncated stream rather than silently
		 * producing incorrect music. */
		if(g_uzesid_capture_overflow){
			truncated = 1;
			trunc_reason |= UZESID_UZSD_FLAG_EVENT_OVERFLOW;
			break;
		}

		if(event_count == 0u){
			pending_empty_ticks++;
		}else{
			event_size = (u32)(2u + ((u16)event_count * 2u));
			required = event_size + 1u; /* Always reserve the stream terminator. */
			if(pending_empty_ticks)
				required += 1u + uleb128_size(pending_empty_ticks);
			if(write_ofs + required > temp_end){
				truncated = 1;
				trunc_reason |= UZESID_UZSD_FLAG_CAPACITY;
				break;
			}
			if(pending_empty_ticks){
				emit_tick_skip_run(&write_ofs, temp_end, pending_empty_ticks);
				committed_ticks += pending_empty_ticks;
				pending_empty_ticks = 0;
			}
			emit_tick_interleaved(&write_ofs, temp_end, UZESID_CAPTURE_EVENTS, event_count);
			committed_ticks++;
		}

		if((attempted_ticks & 255u) == 0u)
			UzeSidCaptureProgress(attempted_ticks, total_ticks);
	}

	if(pending_empty_ticks){
		u32 required = 1u + uleb128_size(pending_empty_ticks) + 1u;
		if(write_ofs + required <= temp_end){
			emit_tick_skip_run(&write_ofs, temp_end, pending_empty_ticks);
			committed_ticks += pending_empty_ticks;
		}else{
			truncated = 1;
			trunc_reason |= UZESID_UZSD_FLAG_CAPACITY;
		}
	}
	if(!emit_stream_end(&write_ofs, temp_end))
		return 0;
	if(attempted_ticks < total_ticks)
		truncated = 1;
	/* The header buffer doubles as the per-tick compact register scratch.
	 * Reload the serialized original before updating its final counters. */
	{
		u8 bank;
		u16 addr;
		emu_ofs_to_bank_addr(header_ofs, &bank, &addr);
		SpiRamReadInto(bank, addr, header, UZESID_UZSD_HEADER_SIZE);
	}
	if(truncated)
		header[7] |= (u8)(UZESID_UZSD_FLAG_TRUNCATED | trunc_reason);
	capture_wr32(header + 12, (u32)((committed_ticks * 1000u + ((u32)tick_hz / 2u)) / (u32)tick_hz));
	capture_wr32(header + 16, committed_ticks);
	capture_wr32(header + 20, write_ofs - (header_ofs + UZESID_UZSD_HEADER_SIZE));
	temp_write(header_ofs, header, UZESID_UZSD_HEADER_SIZE);
	UzeSidCaptureProgress(attempted_ticks, total_ticks);
	if(out_total_size) *out_total_size = write_ofs - header_ofs;
	if(out_song_length_ms) *out_song_length_ms = UzesidRd32(header + 12);
	if(out_tick_hz) *out_tick_hz = tick_hz;
	return (committed_ticks != 0u);
}
