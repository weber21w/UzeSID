#include "UzeSID.h"

#ifndef UZESID_NOINLINE
#define UZESID_NOINLINE __attribute__((noinline))
#endif
#include "Emulation.h"

extern BYTE rcv_spi();

int number_of_songs = 1;
int current_song = 0;
u16 play_adr = 0;
cycle_t cycles_per_second;
u16 cia_timer;

#ifdef UZESID_HAVE_PFF
#endif

/*
 * Uzebox-only SID core plus minimal raw PSID loading/capture helpers.
 */

/* Exported song state placeholders / metadata */

u32 f_rand_seed = 1;
u8 k_gain_q12_4_mv[3] = {0,0,0};
static u16 sid_cycles;
static int speed_adjust;
static u16 k_q8_8;
#if UZESID_SYNTH_DUP == 2u
static u8 sid_interp_prev = 128u;
static u8 sid_interp_valid;
#endif
static u8 sid_env_phase;
/* Register streams often write both bytes of a frequency, or volume several
 * times, in one 50/60 Hz tick.  No audio is generated between those writes,
 * so defer derived phase/gain calculations until the complete ordered batch
 * has been applied.  Bits 0..2 mark dirty voice frequencies; bit 7 marks the
 * global SID-volume gain. */
#define SID_DIRTY_FREQ(v) ((u8)(1u << (v)))
#define SID_DIRTY_GAIN    0x80u
static u8 sid_batch_active;
static u8 sid_derived_dirty;
/* Raw 6510 emulation and cached playback are mutually exclusive.  Reuse the
 * 512-byte emulation/database work buffer as the 262-byte audio staging frame;
 * the frame is copied to the inactive mixer bank before any UI/database work
 * can touch this storage. */
#define sid_frame_staging g_uzesid_workbuf


typedef struct voice_t voice_t;
struct voice_t {
	u8  wave;
	u8  eg_state;
	voice_t *mod_by;
	voice_t *mod_to;

	u32 count;
	u32 add;

	u16 freq;
	u16 pw;

	u16 a_add;
	u16 d_sub;
	u16 s_level;
	u16 r_sub;
	u16 eg_level;
	u8  amp;

	u32 noise;

	u8 gate;
	u8 ring;
	u8 test;
	u8 filter;

	u8 sync;
	u8 mute;
};

struct osid_t {
	voice_t voice[3];
	u8  regs[32];
	u8  last_written_byte;
	u8  volume;
};

static osid_t sid_instance;
static osid_t *sid = &sid_instance;

enum { WAVE_NONE, WAVE_TRI, WAVE_SAW, WAVE_RECT, WAVE_NOISE };
enum { EG_IDLE, EG_ATTACK, EG_DECAY, EG_RELEASE };

/* Audio-rate lookup tables trade plentiful flash for AVR cycles. */
static const u8 sid_wave_mode_map[16] PROGMEM = {
	0x00,0x01,0x02,0x02,0x03,0x02,0x02,0x02,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
static void osid_init(osid_t *S, int n)
{
	(void)n;
	S->voice[0].mod_by = &S->voice[2];
	S->voice[1].mod_by = &S->voice[0];
	S->voice[2].mod_by = &S->voice[1];
	S->voice[0].mod_to = &S->voice[1];
	S->voice[1].mod_to = &S->voice[2];
	S->voice[2].mod_to = &S->voice[0];
}

static inline u8 clamp_gain_u8(u16 gain)
{
	return (gain > 255u) ? 255u : (u8)gain;
}

static inline void update_voice_amp(voice_t *v, u8 gain)
{
	u8 env = (u8)(v->eg_level >> 8);
	/* SID volume, envelope, per-channel gain, and the user master volume all
	 * change far more slowly than oscillator phase.  Cache their product here
	 * so the audio-rate loop needs only the waveform x amplitude multiply. */
#ifdef __AVR__
	u8 amp;
	/* Exact equivalent of floor(floor(env*gain/256)*master/64), using the
	 * hardware multiply result bytes directly. */
	asm volatile(
		"mul %1,%2\n\t"
		"mov %0,r1\n\t"
		"clr __zero_reg__\n\t"
		"mul %0,%3\n\t"
		"mov %0,r1\n\t"
		"lsl r0\n\t"
		"rol %0\n\t"
		"lsl r0\n\t"
		"rol %0\n\t"
		"clr __zero_reg__\n\t"
		: "=&r" (amp)
		: "r" (env), "r" (gain), "r" (masterVolume)
		: "r0", "r1"
	);
	v->amp = amp;
#else
	{
		u16 amp = (u16)(((u16)env * gain) >> 8);
		v->amp = (u8)((amp * masterVolume) >> 6);
	}
#endif
}

static void update_sid_gains(osid_t *S)
{
	u8 i;
	voice_t *v = &S->voice[0];
	for(i = 0; i < 3u; i++, v++){
		k_gain_q12_4_mv[i] = clamp_gain_u8((u16)k_gain_q12_4[i] * S->volume);
		update_voice_amp(v, k_gain_q12_4_mv[i]);
	}
}

void osid_reset(osid_t *S)
{
	int v;
	memset(S->regs, 0, sizeof(S->regs));
	S->last_written_byte = 0;
	S->volume = 15;
	S->regs[24] = 0x0f;

	for(v = 0; v < 3; v++){
		S->voice[v].wave = WAVE_NONE;
		S->voice[v].eg_state = EG_IDLE;
		S->voice[v].count = 0;
		S->voice[v].add = 0;
		S->voice[v].freq = 0;
		S->voice[v].pw = 0;
		S->voice[v].eg_level = 0;
		S->voice[v].amp = 0;
		S->voice[v].s_level = 0;
		S->voice[v].a_add = eg_table[0];
		S->voice[v].d_sub = eg_table[0];
		S->voice[v].r_sub = eg_table[0];
		S->voice[v].gate = 0;
		S->voice[v].ring = 0;
		S->voice[v].test = 0;
		S->voice[v].filter = 0;
		S->voice[v].sync = 0;
		S->voice[v].mute = 0;
		/* The physical SID noise LFSR powers up non-zero.  A zero state never
		 * advances and was one source of incorrect percussion timbre. */
		S->voice[v].noise = 0x007ffff8UL;
	}
#if UZESID_SYNTH_DUP == 2u
	sid_interp_prev = 128u;
	sid_interp_valid = 0u;
#endif
	sid_env_phase = 0u;
	sid_batch_active = 0u;
	sid_derived_dirty = 0u;
	update_sid_gains(S);
}

static inline u32 freq_to_add(u16 freq)
{
	/* Keep the 24-bit SID phase in bits 31..8 of a natural-overflowing u32.
	 * The low byte remains zero so this is exactly the previous 24-bit step,
	 * merely left-aligned.  Natural 32-bit overflow replaces a mask in every
	 * voice/sample iteration. */
	return ((u32)freq * k_q8_8) & 0xffffff00UL;
}

static void sid_begin_register_batch(void)
{
	sid_derived_dirty = 0u;
	sid_batch_active = 1u;
}

static void sid_end_register_batch(void)
{
	u8 dirty = sid_derived_dirty;
	u8 i;

	/* Clear batching first so any future helper that writes a register cannot
	 * accidentally leave another deferred update pending. */
	sid_batch_active = 0u;
	sid_derived_dirty = 0u;
	for(i = 0u; i < 3u; i++){
		if(dirty & SID_DIRTY_FREQ(i))
			sid->voice[i].add = freq_to_add(sid->voice[i].freq);
	}
	if(dirty & SID_DIRTY_GAIN)
		update_sid_gains(sid);
}

static void SIDClockFreqChanged(void)
{
	static const u16 div[16] = { 9,32,63,95,149,220,267,313,392,977,1954,3126,3906,11720,19531,31251 };
	u32 envelope_numerator;
	u8 i;

	sid_cycles = (u16)(cycles_per_second / EMU_RATE_HZ);
	k_q8_8 = (u16)(((u32)cycles_per_second << 8) / EMU_RATE_HZ);

	/* Envelopes are updated once per UZESID_ENV_DECIMATE synth samples.  Scale
	 * each Q0.16 step by the same factor so attack/decay/release time remains
	 * unchanged while most per-sample ADSR work disappears. */
	envelope_numerator = ((u32)sid_cycles << 8) * UZESID_ENV_DECIMATE;
	for(i = 0; i < 16; i++){
		u32 step = envelope_numerator / div[i];
		eg_table[i] = (step > 0xffffUL) ? 0xffffu : (u16)step;
	}

	osid_write(sid, 0, sid->regs[0], 0, 0);
	osid_write(sid, 7, sid->regs[7], 0, 0);
	osid_write(sid, 14, sid->regs[14], 0, 0);
}

void SIDSetClockHz(u32 clock_hz)
{
	/* Accept the normal PAL/NTSC SID range and ignore corrupt cache headers. */
	if(clock_hz < 900000UL || clock_hz > 1100000UL)
		return;
	if(cycles_per_second == clock_hz)
		return;
	cycles_per_second = clock_hz;
	SIDClockFreqChanged();
}

void SIDInit(void)
{
	cycles_per_second = SID_DEFAULT_CLOCK_HZ;
	speed_adjust = 100;
	CPUInit();
	MemoryInit();
	osid_init(sid, 0);
	SIDClockFreqChanged();
	osid_reset(sid);
	g_uzesid_psid_loaded = 0;
}

void SIDReset(cycle_t now)
{
	(void)now;
	osid_reset(sid);
}

u32 osid_read(osid_t *S, u32 adr, cycle_t now)
{
	(void)now;
	switch(adr){
		case 0x19:
		case 0x1a:
			S->last_written_byte = 0;
			return 0xff;
		case 0x1b:
		case 0x1c:
			S->last_written_byte = 0;
			return f_rand();
		default: {
			u8 ret = S->last_written_byte;
			S->last_written_byte = 0;
			return ret;
		}
	}
}

static inline void cap_note(u8 reg, u8 val)
{
#if UZESID_ENABLE_CAPTURE
	UzesidCaptureNoteWrite(reg, val);
#else
	(void)reg;
	(void)val;
#endif
}

void osid_write(osid_t *S, u32 adr, u32 byte, cycle_t now, u8 rmw)
{
	int v;
	(void)now;
	(void)rmw;

	if((adr & 0x1f) < 0x1d)
		adr &= 0x1f;

	S->last_written_byte = S->regs[adr] = (u8)byte;
	cap_note((u8)adr, (u8)byte);
#if UZESID_ENABLE_CAPTURE
	/* During pre-emulation the ordered register transitions are captured, but
	 * oscillator/envelope synthesis is unnecessary until stream playback. */
	if(g_uzesid_capture_enabled)
		return;
#endif
	v = (int)(adr / 7);

	switch(adr){
		case 0:
		case 7:
		case 14:
			S->voice[v].freq = (u16)((S->voice[v].freq & 0xff00u) | (u16)byte);
			if(sid_batch_active)
				sid_derived_dirty |= SID_DIRTY_FREQ(v);
			else
				S->voice[v].add = freq_to_add(S->voice[v].freq);
			break;

		case 1:
		case 8:
		case 15:
			S->voice[v].freq = (u16)((S->voice[v].freq & 0x00ffu) | ((u16)byte << 8));
			if(sid_batch_active)
				sid_derived_dirty |= SID_DIRTY_FREQ(v);
			else
				S->voice[v].add = freq_to_add(S->voice[v].freq);
			break;

		case 2:
		case 9:
		case 16:
			S->voice[v].pw = (u16)((S->voice[v].pw & 0x0f00u) | (u16)byte);
			break;
		case 3:
		case 10:
		case 17:
			S->voice[v].pw = (u16)((S->voice[v].pw & 0x00ffu) | (((u16)byte & 0x0fu) << 8));
			break;

		case 4:
		case 11:
		case 18:
			S->voice[v].wave = pgm_read_byte(&sid_wave_mode_map[(byte >> 4) & 0x0f]);
			if((byte & 1u) != S->voice[v].gate){
				if(byte & 1u)
					S->voice[v].eg_state = EG_ATTACK;
				else if(S->voice[v].eg_state != EG_IDLE)
					S->voice[v].eg_state = EG_RELEASE;
				/* Apply a gate transition on the next sample rather than waiting up
				 * to a complete decimation block. */
				sid_env_phase = (u8)(UZESID_ENV_DECIMATE - 1u);
			}
			S->voice[v].gate = (u8)(byte & 1u);
			S->voice[v].mod_by->sync = (u8)(byte & 2u);
			S->voice[v].ring = (u8)(byte & 4u);
			S->voice[v].test = (u8)(byte & 8u);
			if(S->voice[v].test)
				S->voice[v].count = 0;
			break;

		case 5:
		case 12:
		case 19:
			S->voice[v].a_add = eg_table[(byte >> 4) & 0x0f];
			S->voice[v].d_sub = eg_table[byte & 0x0f];
			break;

		case 6:
		case 13:
		case 20:
			S->voice[v].s_level = (u16)((byte >> 4) & 0x0f) << 12;
			S->voice[v].r_sub = eg_table[byte & 0x0f];
			break;

		case 24:
			S->volume = (u8)(byte & 0x0f);
			if(sid_batch_active)
				sid_derived_dirty |= SID_DIRTY_GAIN;
			else
				update_sid_gains(S);
			break;
		default:
			break;
	}
}

u32 sid_read(u32 adr, cycle_t now)
{
	return osid_read(sid, adr - 0xd400u, now);
}

void sid_write(u32 adr, u32 byte, cycle_t now, u8 rmw)
{
	osid_write(sid, adr - 0xd400u, byte, now, rmw);
}

static inline void sid_noise_clock(voice_t *v)
{
	u32 n = v->noise;
	u8 feedback = (u8)(((n >> 22) ^ (n >> 17)) & 1u);
	v->noise = ((n << 1) | feedback) & 0x007fffffUL;
}

static inline u8 sid_noise_byte(u32 n)
{
	/* Exact output taps from the 23-bit SID noise shift register.  This saves
	 * 640 bytes of lookup tables; it is evaluated only for a noise voice. */
	u8 lo = (u8)n;
	u8 mid = (u8)(n >> 8);
	u8 hi = (u8)(n >> 16);
	return (u8)(((lo >> 2) & 0x01u) |
	            ((lo >> 3) & 0x02u) |
	            ((lo >> 5) & 0x04u) |
	            ((mid     ) & 0x08u) |
	            ((mid >> 1) & 0x10u) |
	            ((hi << 5) & 0x20u) |
	            ((hi << 2) & 0x40u) |
	            ((hi << 1) & 0x80u));
}

static inline s8 voice_wave8(voice_t *v)
{
	u32 phase = v->count;
	u8 top = (u8)(phase >> 24);

	switch(v->wave){
		case WAVE_TRI: /* triangle, optionally ring-modulated */
		{
			u8 t;
			if(v->ring && (v->mod_by->count & 0x80000000UL))
				top ^= 0x80u;
			t = (u8)(top & 0x7fu);
			if(top & 0x80u)
				t = (u8)(0x7fu - t);
			return (s8)((u8)(t << 1) - 128u);
		}
		case WAVE_SAW:
			return (s8)(top ^ 0x80u);
		case WAVE_RECT: /* pulse; threshold uses the upper 12 phase bits */
			return (((u16)(phase >> 20)) < v->pw) ? 127 : -128;
		case WAVE_NOISE:
			return (s8)(sid_noise_byte(v->noise) ^ 0x80u);
		default:
			return 0;
	}
}

static inline void update_envelope(voice_t *v)
{
	switch(v->eg_state){
		case EG_ATTACK: {
			u16 old = v->eg_level;
			v->eg_level = (u16)(old + v->a_add);
			if(v->eg_level < old){
				v->eg_level = 0xffffu;
				v->eg_state = EG_DECAY;
			}
			break;
		}
		case EG_DECAY:
			if(v->eg_level <= v->s_level ||
			   (u16)(v->eg_level - v->s_level) <= v->d_sub)
				v->eg_level = v->s_level;
			else
				v->eg_level = (u16)(v->eg_level - v->d_sub);
			break;
		case EG_RELEASE:
			if(v->eg_level <= v->r_sub){
				v->eg_level = 0;
				v->eg_state = EG_IDLE;
			}else{
				v->eg_level = (u16)(v->eg_level - v->r_sub);
			}
			break;
		default:
			break;
	}
}

static inline void update_all_envelopes(osid_t *S)
{
	u8 i;
	voice_t *v = &S->voice[0];
	for(i = 0; i < 3u; i++, v++){
		if(v->eg_state != EG_IDLE)
			update_envelope(v);
		update_voice_amp(v, k_gain_q12_4_mv[i]);
	}
}

static inline s8 sid_scale_wave_amp(s8 wave, u8 amp)
{
#ifdef __AVR__
	s8 scaled;
	/* AVR has a native signed-by-unsigned 8x8 multiply.  Only product bits
	 * 10..15 survive the mix scaling, so use the high product byte directly
	 * instead of promoting both operands to 16 bits and invoking several
	 * multiply instructions. */
	asm volatile(
		"mulsu %1,%2\n\t"
		"mov %0,r1\n\t"
		"clr __zero_reg__\n\t"
		"asr %0\n\t"
		"asr %0\n\t"
#if UZESID_MIX_SHIFT == 1u
		"asr %0\n\t"
#endif
		: "=&r" (scaled)
		: "a" (wave), "a" (amp)
		: "r0", "r1"
	);
	return scaled;
#else
	return (s8)(((s16)wave * (u16)amp) >> (10u + UZESID_MIX_SHIFT));
#endif
}

static inline s16 calc_voice_sample(voice_t *v)
{
	u32 old_phase;
	u32 new_phase;

	if(v->mute || v->test)
		return 0;

	old_phase = v->count;
	new_phase = old_phase + v->add;
	v->count = new_phase;

	if(new_phase < old_phase && v->sync)
		v->mod_to->count = 0;

	if(v->wave == WAVE_NOISE){
		u32 p = old_phase & 0x0fffffffUL;
		u8 clocks = (u8)(((p + v->add + 0x08000000UL) >> 28) -
		                 ((p + 0x08000000UL) >> 28));
		while(clocks-- != 0u)
			sid_noise_clock(v);
	}

	if(v->amp != 0u && v->wave != WAVE_NONE)
		return (s16)sid_scale_wave_amp(voice_wave8(v), v->amp);
	return 0;
}

static s16 calc_sid_sample(osid_t *S)
{
	s16 sum_output;

	if(++sid_env_phase >= UZESID_ENV_DECIMATE){
		sid_env_phase = 0u;
		update_all_envelopes(S);
	}

	/* Exactly three SID voices: explicit calls avoid loop/index arithmetic and
	 * let the compiler keep each voice base address constant. */
	sum_output = calc_voice_sample(&S->voice[0]);
	sum_output = (s16)(sum_output + calc_voice_sample(&S->voice[1]));
	sum_output = (s16)(sum_output + calc_voice_sample(&S->voice[2]));
	return sum_output;
}

static inline u8 q8u(s16 mix)
{
	s16 sample = (s16)(mix + 128);
	if(sample < 0) sample = 0;
	if(sample > 255) sample = 255;
	return (u8)sample;
}

static void calc_buffer(u8 *buf, int count)
{
#if UZESID_SYNTH_DUP == 1u
	/* Experimental native 15.72 kHz synthesis.  Every DAC sample advances all
	 * three SID oscillators; no reconstruction filter is required. */
	while(count-- > 0)
		*buf++ = q8u(calc_sid_sample(sid));
#else
	u16 pairs = (u16)count >> 1;

	/* Default: synthesize at 7.86 kHz, then linearly reconstruct the
	 * intermediate 15.72 kHz sample. */
	while(pairs-- != 0u){
		s16 mix = calc_sid_sample(sid);
		u8 current;
		current = q8u(mix);
		if(!sid_interp_valid){
			sid_interp_prev = current;
			sid_interp_valid = 1u;
		}
		*buf++ = (u8)(((u16)sid_interp_prev + (u16)current + 1u) >> 1);
		*buf++ = current;
		sid_interp_prev = current;
	}
#endif
}

void cia_tl_write(u8 byte)
{
	cia_timer = (u16)((cia_timer & 0xff00u) | byte);
}

void cia_th_write(u8 byte)
{
	cia_timer = (u16)((cia_timer & 0x00ffu) | ((u16)byte << 8));
}

void SIDExit(void)
{
}

void SIDCalcBuffer(u8 *buf, int count)
{
	calc_buffer(buf, count);
}

void SIDExecute(void)
{
	EmulationUpdatePlayAdr();
	if(play_adr != 0)
		CPUExecute(play_adr, 0, 0, 0, 1000000);
}

void SIDSetReplayFreq(int freq)
{
	if(freq <= 0)
		return;
	cia_timer = (u16)(cycles_per_second / (u32)freq - 1u);
}

void SIDAdjustSpeed(int percent)
{
	if(percent < 1)
		percent = 1;
	speed_adjust = percent;
	(void)speed_adjust;
}

void SIDWriteRegister(int reg, u8 val)
{
	if(reg < 0)
		return;
	osid_write(sid, (u32)(reg & 0x7f), val, 0, 0);
}

void UzeSID_CopyRegs(u8 *dst, u8 count)
{
	u8 i;
	if(count > 128u) count = 128u;
	for(i = 0; i < count; i++) dst[i] = sid->regs[i];
}

u64 GetTicks_usec(void)
{
	return 0;
}

void Delay_usec(u32 usec)
{
	(void)usec;
}

/* ---- player runtime ---- */
static void player_apply_init(const UzesidPlayer *pl)
{
	u8 i;

	if(pl == 0 || pl->sink.write_reg == 0)
		return;
	if(pl->sink.reset != 0)
		pl->sink.reset(pl->sink.user);
	if(pl->sink.begin_batch != 0)
		pl->sink.begin_batch(pl->sink.user);
	for(i = 0; i < UZESID_UZSD_INIT_REG_COUNT; i++)
	{
		if(i == 24)
			continue;
		pl->sink.write_reg(pl->sink.user, i, pl->stream.header.init_regs[i]);
	}
	pl->sink.write_reg(pl->sink.user, 24, pl->stream.header.init_regs[24]);
	if(pl->sink.end_batch != 0)
		pl->sink.end_batch(pl->sink.user);
}

/* Schedule SID register ticks on 131 evenly spaced boundaries per frame.
 * Both synthesis modes still emit all 262 DAC samples; native mode simply
 * advances the SID oscillator twice inside each timing pair.  Keeping the
 * player scheduler pair-based avoids a larger second hot path in flash. */
#define UZESID_AUDIO_PAIR_RATE ((u16)(SAMPLE_RATE / 2UL))
#define UZESID_AUDIO_PAIRS_PER_FRAME ((u8)(UZESID_FRAME_SAMPLES / 2u))

int UzesidPlayerRestart(UzesidPlayer *pl)
{
	if(pl == 0)
		return -1;
	if(UzesidUzsdRestart(&pl->stream) != 0)
		return -1;
	pl->tick_hz = pl->stream.header.tick_hz;
	if(pl->tick_hz == 0u)
		pl->tick_hz = 60u;
	SIDSetClockHz(pl->stream.header.clock_hz);
	/* Apply the first player tick at the beginning of the first output frame. */
	pl->tick_phase = UZESID_AUDIO_PAIR_RATE;
	pl->last_error = 0;
	player_apply_init(pl);
	return 0;
}


int UzesidPlayerOpenStream(UzesidPlayer *pl, const UzesidUzsdStream *stream, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop)
{
	if(pl == 0 || stream == 0 || sink == 0 || sink->write_reg == 0)
		return -1;
	memset(pl, 0, sizeof(*pl));
	pl->stream = *stream;
	(void)entry;
	pl->sink = *sink;
#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
	pl->direct_sid_sink =
		(sink->begin_batch == UzesidUzeSidSinkBeginBatch &&
		 sink->write_reg == UzesidUzeSidSinkWriteReg &&
		 sink->end_batch == UzesidUzeSidSinkEndBatch) ? 1u : 0u;
#endif
	pl->auto_loop = auto_loop ? 1u : 0u;
	pl->enabled = 1;
	if(UzesidPlayerRestart(pl) != 0)
	{
		pl->enabled = 0;
		return -1;
	}
	return 0;
}

int UzesidPlayerOpenSpiTemp(UzesidPlayer *pl, u32 base_offset, u32 total_size, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop)
{
	UzesidUzsdStream stream;
	if(UzesidUzsdOpenFromSpi(&stream, base_offset, total_size) != 0)
		return -1;
	return UzesidPlayerOpenStream(pl, &stream, entry, sink, auto_loop);
}

int UzesidPlayerOpenCachedEntry(UzesidPlayer *pl, const UzesidUsdc *usdc, const UzesidUsdcEntry *entry, const UzesidSidSink *sink, u8 auto_loop)
{
	UzesidUzsdStream stream;
	if(pl == 0 || usdc == 0 || entry == 0 || sink == 0 || sink->write_reg == 0)
		return -1;
	if(UzesidUzsdOpenFromEntry(&stream, usdc, entry) != 0)
		return -1;
	return UzesidPlayerOpenStream(pl, &stream, entry, sink, auto_loop);
}

int UzesidPlayerOpenCachedByMd5(UzesidPlayer *pl, const UzesidUsdc *usdc, const u8 md5[16], u16 subtune_index, const UzesidSidSink *sink, u8 auto_loop)
{
	UzesidUsdcEntry entry;
	if(pl == 0 || usdc == 0 || md5 == 0 || sink == 0)
		return -1;
	if(UzesidUsdcFindEntry(usdc, md5, subtune_index, &entry, 0) != 0)
		return 1;
	return UzesidPlayerOpenCachedEntry(pl, usdc, &entry, sink, auto_loop);
}

void UzesidPlayerStop(UzesidPlayer *pl)
{
	if(pl == 0)
		return;
	pl->enabled = 0;
}

#if !defined(UZESID_DIRECT_SID_ONLY) || !(UZESID_DIRECT_SID_ONLY)
static __attribute__((noinline)) int player_apply_generic_tick(UzesidPlayer *pl, u8 *ended_out)
{
	UzesidUzsdTick tick;
	u8 i;
	int rc = UzesidUzsdNextTick(&pl->stream, &tick);

	if(ended_out != 0)
		*ended_out = tick.ended;
	if(rc != 0 || tick.ended)
		return rc;
	if(tick.count != 0u)
	{
		if(pl->sink.begin_batch != 0)
			pl->sink.begin_batch(pl->sink.user);
		for(i = 0; i < tick.count; i++)
			pl->sink.write_reg(pl->sink.user, tick.reg[i], tick.val[i]);
		if(pl->sink.end_batch != 0)
			pl->sink.end_batch(pl->sink.user);
	}
	return 0;
}
#endif

static int player_apply_one_tick(UzesidPlayer *pl)
{
	u8 ended = 0;
	int rc;

#if defined(UZESID_DIRECT_SID_ONLY) && (UZESID_DIRECT_SID_ONLY)
	rc = UzesidUzsdApplyNextTickToSid(&pl->stream, pl->sink.user, &ended);
#else
	if(pl->direct_sid_sink)
		rc = UzesidUzsdApplyNextTickToSid(&pl->stream, pl->sink.user, &ended);
	else
		rc = player_apply_generic_tick(pl, &ended);
#endif
	if(rc < 0)
	{
		pl->last_error = 1;
		pl->enabled = 0;
		return UZESID_PLAYER_FRAME_ERROR;
	}
	if(rc > 0 || ended)
	{
		if(!pl->auto_loop)
		{
			pl->enabled = 0;
			return UZESID_PLAYER_FRAME_STOPPED;
		}
		pl->loop_count++;
		if(UzesidPlayerRestart(pl) != 0)
		{
			pl->last_error = 1;
			pl->enabled = 0;
			return UZESID_PLAYER_FRAME_ERROR;
		}
		/* Restart applied the stream's initial SID state at this tick boundary.
		 * The first play-routine tick of the new loop is due one tick later. */
		pl->tick_phase = 0u;
		return UZESID_PLAYER_FRAME_LOOPED;
	}
	return UZESID_PLAYER_FRAME_TICK;
}

/* Advance one video frame in 7.86 kHz timing-pair units.  CIA-driven
 * tunes can execute several play ticks per 60 Hz frame; rendering the
 * intervals between ticks preserves fast arpeggios and gate changes.  A
 * native-15 build still synthesizes both output samples in every pair. */
static int player_advance_pairs(UzesidPlayer *pl, u8 *buf, u8 pairs)
{
	int result = UZESID_PLAYER_FRAME_NO_TICK;

	if(pl == 0)
		return UZESID_PLAYER_FRAME_ERROR;
	if(!pl->enabled)
		return UZESID_PLAYER_FRAME_STOPPED;
	while(pairs != 0u)
	{
		while(pl->tick_phase >= UZESID_AUDIO_PAIR_RATE)
		{
			int rc;
			pl->tick_phase = (u16)(pl->tick_phase - UZESID_AUDIO_PAIR_RATE);
			rc = player_apply_one_tick(pl);
			if(rc == UZESID_PLAYER_FRAME_ERROR || rc == UZESID_PLAYER_FRAME_STOPPED)
			{
				if(buf != 0 && pairs != 0u)
					SIDCalcBuffer(buf, (int)(pairs << 1));
				return rc;
			}
			result = rc;
		}
		{
			u8 run = 0u;
			do {
				pl->tick_phase = (u16)(pl->tick_phase + pl->tick_hz);
				run++;
			} while(run < pairs && pl->tick_phase < UZESID_AUDIO_PAIR_RATE);
			if(buf != 0)
			{
				SIDCalcBuffer(buf, (int)(run << 1));
				buf += (u16)(run << 1);
			}
			pairs = (u8)(pairs - run);
		}
	}
	return result;
}

int UzesidPlayerStepVideoFrame(UzesidPlayer *pl)
{
	return player_advance_pairs(pl, 0, UZESID_AUDIO_PAIRS_PER_FRAME);
}


/* ---- sid sink ---- */
void UzesidUzeSidSinkReset(void *user)
{
	(void)user;
	SIDReset(0);
}

void UzesidUzeSidSinkBeginBatch(void *user)
{
	(void)user;
	sid_begin_register_batch();
}

void UzesidUzeSidSinkWriteReg(void *user, u8 reg, u8 val)
{
	(void)user;
	SIDWriteRegister((int)reg, val);
}

void UzesidUzeSidSinkEndBatch(void *user)
{
	(void)user;
	sid_end_register_batch();
}

void UzesidUzeSidMakeSink(UzesidSidSink *sink, UzesidUzeSidSinkState *state)
{
	if(sink == 0) return;
	sink->user = state;
	sink->reset = UzesidUzeSidSinkReset;
	sink->begin_batch = UzesidUzeSidSinkBeginBatch;
	sink->write_reg = UzesidUzeSidSinkWriteReg;
	sink->end_batch = UzesidUzeSidSinkEndBatch;
}

/* ---- player shell ---- */
#ifndef BAD_MASK_COUNT
#define BAD_MASK_COUNT 0
#endif

#ifndef UZESID_DB_OFFSET
#define UZESID_DB_OFFSET 61952UL
#endif

#ifndef UZESID_ROM_FILENAME
#define UZESID_ROM_FILENAME "UZESID.UZE"
#endif

#ifndef UZESID_FRAME_SAMPLES
#define UZESID_FRAME_SAMPLES 262
#endif

#ifndef UZESID_LIST_ROWS
#define UZESID_LIST_ROWS 10
#endif
#ifndef UZESID_BROWSER_LIST_WIDTH
#define UZESID_BROWSER_LIST_WIDTH 20
#endif
#ifndef UZESID_BROWSER_PREVIEW_WIDTH
#define UZESID_BROWSER_PREVIEW_WIDTH 25
#endif

#define UZESID_PREFS_MARKER_INDEX 27
#define UZESID_PREFS_VOLUME_INDEX 28
#define UZESID_PREFS_COLOR_INDEX  29
#define UZESID_PREFS_MARKER       0xA5

#define UZESID_INVALID_INDEX 0xffffffffUL

#if UZESID_ENABLE_CACHE_WRITE
#define UZESID_SAVE_IDLE          0u
#define UZESID_SAVE_DATA          1u
#define UZESID_SAVE_COMMIT_BLOCKS 2u
#define UZESID_SAVE_COMMIT_ENTRY  3u
#define UZESID_SAVE_COMMIT_HEADER 4u
#endif

typedef enum UzeSidSource_e {
	UZESID_SRC_NONE = 0,
	UZESID_SRC_CACHE = 1,
	UZESID_SRC_TEMP = 2
} UzeSidSource;


typedef struct UzeSidPlayerState_s {
	UzesidPffContext pff_ctx;
	UzesidReader reader;
	UzesidLidx lidx;
	UzesidUsdc usdc;
	UzesidPlayer player;
	UzesidSidSink sink;
	UzesidUzeSidSinkState sink_state;
	UzesidUsdcEntry entry;
	u32 current_index;
	u8 db_open;
	u8 lidx_ok;
	u8 usdc_ok;
	u8 open_error;
	u8 loaded;
	u8 redraw;
	u8 last_step;
	u8 fast_forward;
	u8 source;
} UzeSidPlayerState;

static UzeSidPlayerState sp;
#if UZESID_ENABLE_CACHE_WRITE
/* Reuse otherwise idle state bytes so cooperative cache saving adds no SRAM. */
#define SP_SAVE_OFFSET (sp.lidx.header.build_unix_time)
#define SP_SAVE_STATE  (sp.last_step)
#define SP_SAVE_ERROR  (sp.open_error)
#endif

/* The PSID loader reuses the existing USDC entry as its 124-byte header
 * scratch area. Fail at compile time if either structure later changes. */
typedef char UzeSidPsidScratchFitsEntry[
	(sizeof(((UzeSidPlayerState *)0)->entry) >= UZESID_PSID_SCRATCH_SIZE) ? 1 : -1];

static u8 ptime_min;
static u8 ptime_sec;
static u8 ptime_frame;
/* declared with the player globals so capture progress can show capacity */

FATFS fs;
u16 oldpad, pad;
u8 play_state = 0;
u8 masterVolume = 64;

#ifndef SONG_TITLE_BUF_LEN
#define SONG_TITLE_BUF_LEN 64
#endif
#ifndef SONG_TITLE_VISIBLE_CHARS
#define SONG_TITLE_VISIBLE_CHARS 20
#endif
#ifndef SONG_TITLE_HOLD_START
#define SONG_TITLE_HOLD_START 120
#endif
#ifndef SONG_TITLE_SCROLL_RATE
#define SONG_TITLE_SCROLL_RATE 8
#endif
#ifndef SONG_TITLE_GAP
#define SONG_TITLE_GAP 3
#endif

static char g_song_title[SONG_TITLE_BUF_LEN];
static u8 g_song_title_len;
static u8 g_song_title_scroll;
static u8 g_song_title_hold;
static u8 g_song_title_tick;
static u32 g_detected_ram;

static void UMPrintChar(u8 x, u8 y, char c);
static void UMPrint(u8 x, u8 y, const char *s);
static void UMPrintRamClip(u8 x, u8 y, const char *s, u8 width);
static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb);
static void InputDeviceHandler(void);
static void UpdateCursor(u8 ylimit);
UZESID_NOINLINE static void PlayerInterface(void);
UZESID_NOINLINE static void CacheSelectWindow(void);
UZESID_NOINLINE static void RawSidSelectWindow(void);
UZESID_NOINLINE static u8 UzeSidImportRawSid(const char *path, s16 requested_song);
UZESID_NOINLINE static u8 ReadSidMetaTitle(const char *path, char *title);
UZESID_NOINLINE static u8 UzeSidLoadEntryDirect(u32 entry_index, const UzesidUsdcEntry *entry, u8 source);
static u8 UzeSidLoadTempCurrent(void);
static u8 UzeSidCopyDbEntryToTemp(const UzesidUsdcEntry *entry);
#if UZESID_ENABLE_CACHE_WRITE
static u8 UzeSidCacheSaveBegin(void);
static void UzeSidCacheSavePump(void);
static void UzeSidCacheSaveAbort(u8 error);
#endif
static u8 ReopenDbFile(void);
static void LoadPreferences(void);
static void SavePreferences(void);
static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h);
static void PollPad(void);
static void UzeSidClearLoadedState(void);
static void UzeSidSetNoSongLoaded(void);
static u8 UzeSidEnsureDbOpen(void);
static u8 UzeSidOpenDb(void);
static void UzeSidBuildSongTitle(const UzesidUsdcEntry *entry);
static void UzeSidPrintSongTitle(u8 x, u8 y, u8 len);
static void UzeSidUpdateSongTitleScroll(void);
static void SpiRamWriteStringEntry(u32 pos, const char *src);
static void SpiRamReadStringEntry(u32 pos, char *dst);

/* Prebuilt-cache paths for bundled diagnostic and test SIDs.  Matching a
 * known filename lets the player open its MD5/subtune entry before loading or
 * executing the PSID, so cached playback exercises only the UZSD decoder and
 * SID synthesizer. */
typedef struct
{
	char name[32];
	u8 md5[16];
	u8 subtunes;
} UzeSidPrebuiltInfo;

#if !defined(UZESID_HOST_CONVERTER)
#include "db-tool/prebuilt_sids.inc"
#endif
#ifndef UZESID_PREBUILT_COUNT
#define UZESID_PREBUILT_COUNT 0u
#endif
#if UZESID_PREBUILT_COUNT < UZESID_RAWLIST_MAX
#define UZESID_CARDLIST_MAX (UZESID_RAWLIST_MAX - UZESID_PREBUILT_COUNT)
#else
#define UZESID_CARDLIST_MAX 0u
#endif

static u8 UzeSidFindPrebuilt(const char *name, u8 md5[16], u8 *subtunes)
{
	u8 n;
	if(name == 0 || md5 == 0 || subtunes == 0)
		return 0;
#if UZESID_PREBUILT_COUNT > 0
	for(n = 0; n < (u8)UZESID_PREBUILT_COUNT; n++){
		u8 i;
		u8 matched = 1;
		for(i = 0; i < sizeof(g_prebuilt_sids[0].name); i++){
			char a = name[i];
			char b = (char)pgm_read_byte(&g_prebuilt_sids[n].name[i]);
			if(a >= 'a' && a <= 'z') a -= 32;
			if(b >= 'a' && b <= 'z') b -= 32;
			if(a != b){ matched = 0; break; }
			if(a == 0) break;
		}
		if(matched){
			for(i = 0; i < 16u; i++)
				md5[i] = pgm_read_byte(&g_prebuilt_sids[n].md5[i]);
			*subtunes = pgm_read_byte(&g_prebuilt_sids[n].subtunes);
			return 1;
		}
	}
#else
	(void)n;
#endif
	return 0;
}


static u8 HasExt(const char *name, const char *ext)
{
	u8 i, j;
	if(name == 0 || ext == 0)
		return 0;
	for(i = 0; name[i] != 0; i++){}
	for(j = 0; ext[j] != 0; j++){}
	if(i < j)
		return 0;
	name += (i - j);
	for(i = 0; ext[i] != 0; i++){
		char a = name[i];
		char b = ext[i];
		if(a >= 'a' && a <= 'z') a -= 32;
		if(b >= 'a' && b <= 'z') b -= 32;
		if(a != b)
			return 0;
	}
	return 1;
}

static u8 UzeSidStrLen(const char *s, u8 max_len)
{
	u8 n = 0;
	if(s == 0)
		return 0;
	while(n < max_len && s[n] != 0)
		n++;
	return n;
}

static void UzeSidTitleResetScroll(void)
{
	g_song_title_len = UzeSidStrLen(g_song_title, (u8)(SONG_TITLE_BUF_LEN - 1));
	g_song_title_scroll = 0;
	g_song_title_hold = SONG_TITLE_HOLD_START;
	g_song_title_tick = 0;
}

static void UzeSidTitleSet(const char *s)
{
	u8 i = 0;
	if(s == 0)
		s = "No song loaded";
	while(i + 1 < SONG_TITLE_BUF_LEN && s[i] != 0){
		g_song_title[i] = s[i];
		i++;
	}
	g_song_title[i] = 0;
	UzeSidTitleResetScroll();
}

static void UzeSidTitleAppendChar(u8 *pos, char c)
{
	if(*pos + 1 >= SONG_TITLE_BUF_LEN)
		return;
	g_song_title[*pos] = c;
	(*pos)++;
	g_song_title[*pos] = 0;
}

static void UzeSidTitleAppendString(u8 *pos, const char *s, u8 max_len)
{
	u8 i;
	if(s == 0)
		return;
	for(i = 0; i < max_len && s[i] != 0; i++){
		if(*pos + 1 >= SONG_TITLE_BUF_LEN)
			break;
		g_song_title[*pos] = s[i];
		(*pos)++;
	}
	g_song_title[*pos] = 0;
}

static void UzeSidSetNoSongLoaded(void)
{
	UzeSidTitleSet("No song loaded");
}

static void UzeSidBuildSongTitle(const UzesidUsdcEntry *entry)
{
	u8 pos = 0;
	u8 have_author;
	u8 have_year;
	if(entry == 0 || entry->title[0] == 0){
		UzeSidSetNoSongLoaded();
		return;
	}
	g_song_title[0] = 0;
	UzeSidTitleAppendString(&pos, entry->title, 32);
	have_author = (entry->author[0] != 0);
	have_year = (entry->released[0] != 0);
	if(have_author || have_year){
		UzeSidTitleAppendString(&pos, " (", 2);
		if(have_author)
			UzeSidTitleAppendString(&pos, entry->author, 32);
		if(have_year){
			if(have_author)
				UzeSidTitleAppendString(&pos, ", ", 2);
			UzeSidTitleAppendString(&pos, entry->released, 16);
		}
		UzeSidTitleAppendChar(&pos, ')');
	}
	UzeSidTitleResetScroll();
}

static void UzeSidPrintSongTitle(u8 x, u8 y, u8 len)
{
	u8 i;
	u8 cycle_len;
	if(g_song_title_len == 0){
		for(i = 0; i < len; i++)
			SetTile(x + i, y, 0);
		return;
	}
	if(g_song_title_len <= len){
		for(i = 0; i < g_song_title_len; i++)
			UMPrintChar(x + i, y, g_song_title[i]);
		while(i < len){
			SetTile(x + i, y, 0);
			i++;
		}
		return;
	}
	cycle_len = (u8)(g_song_title_len + SONG_TITLE_GAP);
	for(i = 0; i < len; i++){
		u8 idx = (u8)(g_song_title_scroll + i);
		while(idx >= cycle_len)
			idx = (u8)(idx - cycle_len);
		if(idx < g_song_title_len)
			UMPrintChar(x + i, y, g_song_title[idx]);
		else
			UMPrintChar(x + i, y, ' ');
	}
}

static void UzeSidUpdateSongTitleScroll(void)
{
	u8 cycle_len;
	if(g_song_title_len <= SONG_TITLE_VISIBLE_CHARS){
		g_song_title_scroll = 0;
		g_song_title_hold = SONG_TITLE_HOLD_START;
		g_song_title_tick = 0;
		return;
	}
	cycle_len = (u8)(g_song_title_len + SONG_TITLE_GAP);
	if(g_song_title_scroll == 0 && g_song_title_hold){
		g_song_title_hold--;
		return;
	}
	g_song_title_tick++;
	if(g_song_title_tick >= SONG_TITLE_SCROLL_RATE){
		g_song_title_tick = 0;
		g_song_title_scroll++;
		if(g_song_title_scroll >= cycle_len){
			g_song_title_scroll = 0;
			g_song_title_hold = SONG_TITLE_HOLD_START;
		}
	}
}

static void SilenceBuffer(void)
{
	u16 i;
	for(i = 0; i < (u16)(UZESID_FRAME_SAMPLES * 2); i++)
		mix_buf[i] = 0x80;
}

/* Called by the blocking raw-SID loader.  Video continues to scan VRAM from
 * interrupts while the foreground code loads, so these stages identify the
 * exact operation without adding WaitVsync calls inside Petit FatFs access. */
void UzeSidLoadProgress(u8 stage)
{
	SilenceBuffer();
	switch(stage){
		case 0: UMPrint(0, 1, PSTR("READING SID HEADER...         ")); break;
		case 1: UMPrint(0, 1, PSTR("LOADING SID DATA...           ")); break;
		case 2: UMPrint(0, 1, PSTR("HASHING SID FILE...           ")); break;
		case 3: UMPrint(0, 1, PSTR("RUNNING SID INIT...           ")); break;
		case 4: UMPrint(0, 1, PSTR("CHECKING SONG CACHE...        ")); break;
		case 5:
			UMPrint(0, 0, PSTR("PRE-EMULATING                 "));
			break;
		default: UMPrint(0, 0, PSTR("STARTING CACHED PLAYBACK...   ")); break;
	}
}

void UzeSidCaptureProgress(u32 captured_ticks, u32 total_ticks)
{
	u8 percent = 0;
	/* Pre-emulation needs only one visible tile row.  Keeping the SPI usage
	 * diagnostics off-screen saves eight rendered scanlines during capture. */
	UMPrint(0, 0, PSTR("PRE-EMULATING       %           "));
	if(total_ticks != 0u)
		percent = (u8)((captured_ticks * 100UL) / total_ticks);
	PrintByte(17, 0, percent, 0);
}

static void UzeSidResetClock(void)
{
	ptime_min = 0;
	ptime_sec = 0;
	ptime_frame = 0;
}

static void UzeSidAdvanceClock(void)
{
	if(++ptime_frame >= 60){
		ptime_frame = 0;
		if(++ptime_sec >= 60){
			ptime_sec = 0;
			if(++ptime_min > 99)
				ptime_min = 99;
		}
	}
}

static void UzeSidApplyUiMetadata(const UzesidUsdcEntry *entry, u8 source)
{
	if(entry == 0)
		return;
	sp.entry = *entry;
	sp.source = source;
	UzeSidBuildSongTitle(entry);
}

static void UzeSidBuildCurrentEntry(UzesidUsdcEntry *entry, const u8 md5[16])
{
	if(entry == 0)
		return;

	/* md5 may alias entry->md5. Fill every other field explicitly rather
	 * than clearing the whole record or allocating a second track record. */
	if(md5 == 0)
		memset(entry->md5, 0, sizeof(entry->md5));
	else if(md5 != entry->md5)
		memcpy(entry->md5, md5, sizeof(entry->md5));
	entry->state = 0;
	entry->type = 0;
	entry->flags = 0;
	entry->usd_size = 0;
	entry->length_ms = 0;
	entry->subtune_index = (u16)current_song;
	entry->subtune_count = (u16)((number_of_songs > 0) ? number_of_songs : 1);
	entry->tick_hz = 0;
	entry->reserved0 = 0;
	entry->first_block = 0;
	entry->block_count = 0;
	/* LoadPSIDFilePff stores the three PSID text fields directly in this
	 * aliased entry before it reuses only the entry prefix as its I/O buffer. */
	entry->title[sizeof(entry->title) - 1] = 0;
	entry->author[sizeof(entry->author) - 1] = 0;
	entry->released[sizeof(entry->released) - 1] = 0;
	entry->entry_crc = 0;
}

static void UzeSidRedrawShell(void)
{
	ClearVram();
	DrawMap(5, 0, (const char *)buttons_map);
#if UZESID_ENABLE_CACHE_WRITE
	if(SP_SAVE_ERROR == 3u)
		UMPrint(5, 2, PSTR("CACHE DIRECTORY FULL"));
	else if(SP_SAVE_ERROR == 4u)
		UMPrint(5, 2, PSTR("CACHE DATABASE FULL "));
	else if(SP_SAVE_ERROR != 0u)
		UMPrint(5, 2, PSTR("CACHE SAVE FAILED   "));
	else
#endif
		UzeSidPrintSongTitle((CONT_BAR_X / 8), (CONT_BAR_Y / 8) + 2, SONG_TITLE_VISIBLE_CHARS);
	UMPrint(PTIME_X, PTIME_Y, PSTR("  :  :  "));
	play_state |= PS_DRAWN;
}

static u8 UzeSidEnsureDbOpen(void)
{
	/* ReopenDbFile() may have selected UZESID.UZE after another Petit FatFs
	 * file without decoding the database headers yet.  Do not treat the
	 * active file alone as proof that LIDX/USDC state is initialized. */
	if(sp.db_open && (sp.lidx_ok || sp.usdc_ok))
		return 0;
	if(UzeSidOpenDb() != 0)
		return 1;
	return (!sp.lidx_ok && !sp.usdc_ok) ? 1 : 0;
}

static u8 UzeSidOpenDb(void)
{
	FRESULT res;
	u8 rc;

	/* The database is optional and may be opened while a raw SPI-temp song is
	 * already playing.  The open helpers fully initialize their own state, so
	 * leave the player, current entry, and temporary stream untouched. */
	sp.db_open = 0;
	sp.lidx_ok = 0;
	sp.usdc_ok = 0;
	sp.open_error = 0;
	UzesidUzeSidMakeSink(&sp.sink, &sp.sink_state);
	res = pf_open(UZESID_ROM_FILENAME);
	if(res != FR_OK)
		return 1;
	UzesidPffInitReader(&sp.reader, &sp.pff_ctx);
	sp.db_open = 1;
	if(fs.fsize < (u32)UZESID_DB_OFFSET + 64UL){
		sp.open_error = 0xE1u;
		return 1;
	}
	rc = UzesidLidxOpen(&sp.lidx, &sp.reader, (u32)UZESID_DB_OFFSET);
	if(rc == 0){
		sp.lidx_ok = 1;
		rc = UzesidUsdcOpen(&sp.usdc, &sp.reader, sp.lidx.next_region_offset);
		if(rc == 0 && sp.usdc.base_offset <= fs.fsize &&
			sp.usdc.header.cache_bytes <= fs.fsize - sp.usdc.base_offset){
			sp.usdc_ok = 1;
		}else{
			if(rc == 0) rc = 0x20u;
			sp.open_error = (u8)(rc + 7);
		}
	}else{
		sp.open_error = (u8)(rc + 1);
	}
	sp.redraw = 1;
	return 0;
}

static u8 UzeSidReadLiveEntryByOrdinal(u16 ordinal, u32 *entry_index, UzesidUsdcEntry *entry)
{
	u32 i;
	u16 live = 0;

	if(!sp.usdc_ok || entry == 0)
		return 1;
	for(i = 0; i < sp.usdc.header.dir_entry_count; i++){
		if(UzesidUsdcReadEntry(&sp.usdc, i, entry) != 0)
			continue;
		if(entry->state != UZESID_USDC_ENTRY_LIVE || entry->type != UZESID_USDC_ENTRY_TYPE_USD)
			continue;
		if(live == ordinal){
			if(entry_index != 0)
				*entry_index = i;
			return 0;
		}
		live++;
	}
	return 1;
}
static u16 UzeSidGetLiveCount(void)
{
	if(!sp.usdc_ok)
		return 0;
	if(sp.usdc.header.live_entries > 65535UL)
		return 65535;
	return (u16)sp.usdc.header.live_entries;
}

static void UzeSidSpiWriteAt(u32 ofs, const u8 *src, u16 len)
{
	while(len != 0u){
		u8 bank = (u8)(ofs >> 16);
		u16 addr = (u16)ofs;
		u16 chunk = len;
		/* A complete 64 KiB remainder is 0x10000 and cannot fit in u16.
		 * Treat bank address zero as having room for this whole request. */
		if(addr != 0u){
			u16 room = (u16)(0x10000UL - (u32)addr);
			if(chunk > room) chunk = room;
		}
		SpiRamWriteFrom(bank, addr, (void *)src, chunk);
		ofs += chunk;
		src += chunk;
		len = (u16)(len - chunk);
	}
}

static u8 UzeSidCopyDbEntryToTemp(const UzesidUsdcEntry *entry)
{
	u32 offset = 0;
	u32 capacity;
	u32 file_offset;
	UINT br;
	u8 x;
	if(entry == 0 || entry->usd_size < UZESID_UZSD_HEADER_SIZE)
		return 1;
	if(g_detected_ram <= UZESID_TEMP_SPI_OFS)
		return 2;
	capacity = g_detected_ram - UZESID_TEMP_SPI_OFS;
	if(entry->usd_size > capacity)
		return 3;

	/* The USDC allocator stores each stream in one contiguous block run.
	 * Seek once, then read sequentially.  Re-seeking for every 512-byte chunk
	 * is needlessly expensive in Petit FatFs, especially in a large .UZE. */
	file_offset = sp.usdc.base_offset + sp.usdc.header.data_offset +
		(entry->first_block << sp.usdc.header.block_shift);
	if(pf_lseek(file_offset) != FR_OK)
		return 4;

	SetRenderingParameters(33, 16);
	for(x = 0; x < 32u; x++){
		SetTile(x, 0, 0);
		SetTile(x, 1, 0);
	}
	UMPrint(0, 0, PSTR("LOADING PREBUILT CACHE..."));
	UMPrint(0, 1, PSTR("COPIED:       K"));
	PrintLong(8, 1, 0);

	while(offset < entry->usd_size){
		u16 chunk = (u16)((entry->usd_size - offset) > 512UL ?
			512u : (entry->usd_size - offset));
		if(pf_read(g_uzesid_workbuf, chunk, &br) != FR_OK || br != chunk)
			return 5;
		while(rcv_spi() != 0xFF);
		UzeSidSpiWriteAt(UZESID_TEMP_SPI_OFS + offset, g_uzesid_workbuf, chunk);
		offset += chunk;
		if((offset & 0x3fffUL) == 0u || offset == entry->usd_size){
			PrintLong(8, 1, offset >> 10);
			WaitVsync(1);
		}
	}
	return 0;
}

UZESID_NOINLINE static u8 UzeSidLoadEntryDirect(u32 entry_index, const UzesidUsdcEntry *entry, u8 source)
{
	if(entry == 0)
		return 1;
	/* entry normally points at the global sp.entry.  Keeping a second 128-byte
	 * copy on the AVR stack during SD and SPI calls needlessly reduced the
	 * interrupt margin and could corrupt VRAM on deep Petit FatFs paths. */
	if(UzeSidCopyDbEntryToTemp(entry) != 0)
		return 2;
	if(UzesidPlayerOpenSpiTemp(&sp.player, UZESID_TEMP_SPI_OFS, entry->usd_size,
			entry, &sp.sink, 1) != 0)
		return 3;
	sp.current_index = entry_index;
	sp.loaded = 1;
	UzeSidApplyUiMetadata(entry, source);
	UzeSidResetClock();
	play_state = PS_LOADED | PS_PLAYING;
	sp.redraw = 1;
	return 0;
}

static u8 UzeSidMd5Present(const u8 md5[16])
{
	u8 i;
	for(i = 0; i < 16u; i++)
		if(md5[i] != 0u)
			return 1;
	return 0;
}

#if UZESID_ENABLE_CACHE_WRITE
static void UzeSidCacheSaveAbort(u8 error)
{
	SP_SAVE_STATE = UZESID_SAVE_IDLE;
	/* A failed CMD24 initiation can leave the card selected because Petit
	 * FatFs aborts before its normal finalize call.  Always release CS and
	 * remount so playback can continue without a soft reset. */
	SD_DESELECT();
	(void)rcv_spi();
	sp.db_open = 0;
	sp.lidx_ok = 0;
	sp.usdc_ok = 0;
	if(pf_mount(&fs) == FR_OK)
		(void)UzeSidOpenDb();
	SP_SAVE_ERROR = error;
	sp.redraw = 1;
}

UZESID_NOINLINE static u8 UzeSidCacheSaveBegin(void)
{
	u32 entry_index;
	u32 block_count;

	SP_SAVE_STATE = UZESID_SAVE_IDLE;
	SP_SAVE_ERROR = 0;
	if(sp.entry.usd_size < UZESID_UZSD_HEADER_SIZE || !UzeSidMd5Present(sp.entry.md5))
		return 1;
	if(ReopenDbFile() != 0 || !sp.usdc_ok)
		return 2;
	if(UzesidUsdcFindEntry(&sp.usdc, sp.entry.md5, (u16)current_song,
			&sp.entry, &entry_index) == 0){
		sp.current_index = entry_index;
		return 0;
	}
	if(UzesidUsdcFindFreeEntry(&sp.usdc, &entry_index) != 0)
		return 3;
	block_count = (sp.entry.usd_size + sp.usdc.block_size - 1u) >> sp.usdc.header.block_shift;
	if(UzesidUsdcFindFreeBlocks(&sp.usdc, block_count, &sp.entry.first_block) != 0)
		return 4;
	sp.entry.block_count = block_count;
	sp.entry.type = UZESID_USDC_ENTRY_TYPE_USD;
	sp.entry.state = UZESID_USDC_ENTRY_LIVE;
	sp.entry.entry_crc = 0;
	sp.current_index = entry_index;
	SP_SAVE_OFFSET = 0;
	SP_SAVE_STATE = UZESID_SAVE_DATA;
	return 0;
}

UZESID_NOINLINE static void UzeSidCacheSavePump(void)
{
	u8 rc;
	if(SP_SAVE_STATE == UZESID_SAVE_IDLE)
		return;

	if(SP_SAVE_STATE == UZESID_SAVE_DATA){
		u16 valid = (u16)((sp.entry.usd_size - SP_SAVE_OFFSET) > 512UL ?
			512u : (sp.entry.usd_size - SP_SAVE_OFFSET));
		if(UzesidSpiReadAt(0, UZESID_TEMP_SPI_OFS + SP_SAVE_OFFSET,
				g_uzesid_workbuf, valid) != 0){
			UzeSidCacheSaveAbort(5);
			return;
		}
		if(valid < 512u)
			memset(g_uzesid_workbuf + valid, 0, (u16)(512u - valid));
		rc = UzesidUsdcWriteData(&sp.usdc, sp.entry.first_block,
			SP_SAVE_OFFSET, g_uzesid_workbuf, 512);
		if(rc != 0){
			UzeSidCacheSaveAbort(6);
			return;
		}
		SP_SAVE_OFFSET += valid;
		if(SP_SAVE_OFFSET >= sp.entry.usd_size)
			SP_SAVE_STATE = UZESID_SAVE_COMMIT_BLOCKS;
		return;
	}

	if(SP_SAVE_STATE == UZESID_SAVE_COMMIT_BLOCKS){
		rc = UzesidUsdcCommitBlocks(&sp.usdc, sp.entry.first_block, sp.entry.block_count);
		if(rc != 0){ UzeSidCacheSaveAbort(7); return; }
		SP_SAVE_STATE = UZESID_SAVE_COMMIT_ENTRY;
		return;
	}
	if(SP_SAVE_STATE == UZESID_SAVE_COMMIT_ENTRY){
		rc = UzesidUsdcWriteEntry(&sp.usdc, sp.current_index, &sp.entry);
		if(rc != 0){ UzeSidCacheSaveAbort(8); return; }
		SP_SAVE_STATE = UZESID_SAVE_COMMIT_HEADER;
		return;
	}
	if(SP_SAVE_STATE == UZESID_SAVE_COMMIT_HEADER){
		sp.usdc.header.live_entries++;
		rc = UzesidUsdcWriteHeader(&sp.usdc);
		if(rc != 0){
			sp.usdc.header.live_entries--;
			UzeSidCacheSaveAbort(9);
			return;
		}
		SP_SAVE_STATE = UZESID_SAVE_IDLE;
		SP_SAVE_ERROR = 0;
		sp.redraw = 1;
	}
}
#endif

static u8 UzeSidLoadTempCurrent(void)
{
	u32 total_size;
	u32 song_length_ms;
	u32 capacity;
	u16 tick_hz;
	u8 rc;

	UzeSidBuildCurrentEntry(&sp.entry, sp.entry.md5);
	if(sp.lidx_ok && UzeSidMd5Present(sp.entry.md5)){
		rc = UzesidLidxGetLengthMs(&sp.lidx, sp.entry.md5,
			(u16)current_song, &song_length_ms);
		if(rc != 0)
			song_length_ms = 180000UL;
	}else{
		song_length_ms = 180000UL;
	}
	if(g_detected_ram <= UZESID_TEMP_SPI_OFS)
		return 1;
	capacity = g_detected_ram - UZESID_TEMP_SPI_OFS;
	SetRenderingParameters(33, 8);
	UzeSidLoadProgress(5);
	if(!UzesidCaptureCurrentSongToSpi(UZESID_TEMP_SPI_OFS, capacity,
			song_length_ms, &total_size, &sp.entry.length_ms, &tick_hz))
		return 1;
	sp.entry.usd_size = total_size;
	sp.entry.tick_hz = tick_hz;
	/* Read the serialized flags before database writeback so the persistent
	 * directory records truncation correctly. */
	if(UzesidPlayerOpenSpiTemp(&sp.player, UZESID_TEMP_SPI_OFS, total_size,
			&sp.entry, &sp.sink, 1) != 0)
		return 2;
	sp.entry.flags = sp.player.stream.header.flags;
	sp.current_index = UZESID_INVALID_INDEX;
	/* Keep raw-SID context after capture/writeback so Previous/Next continues
	 * to select subtunes.  Loading the same raw SID after reboot can still use
	 * its persistent stream without changing that navigation behavior. */
	UzeSidApplyUiMetadata(&sp.entry, UZESID_SRC_TEMP);

#if UZESID_ENABLE_CACHE_WRITE
	{
		u8 save_rc = UzeSidCacheSaveBegin();
		if(save_rc != 0){
			SP_SAVE_ERROR = save_rc;
			sp.redraw = 1;
		}else if(SP_SAVE_STATE != UZESID_SAVE_IDLE){
			u8 shown = 0xffu;
			/* Cache writeback owns the foreground until every data block and
			 * directory/header commit is complete.  The mixer keeps consuming
			 * silence, so a newly captured song cannot start and stutter while
			 * the SD card is still being updated. */
			SilenceBuffer();
			UMPrint(0, 0, PSTR("SAVING CACHE      %             "));
			while(SP_SAVE_STATE != UZESID_SAVE_IDLE){
				u8 percent;
				UzeSidCacheSavePump();
				if(SP_SAVE_STATE == UZESID_SAVE_DATA && sp.entry.usd_size != 0u)
					percent = (u8)((SP_SAVE_OFFSET * 100UL) / sp.entry.usd_size);
				else
					percent = 100u;
				if(percent != shown){
					shown = percent;
					PrintByte(15, 0, percent, 0);
					WaitVsync(1);
				}
			}
		}
	}
#endif
	/* Only publish the new player state after cache writeback has completed
	 * (or failed cleanly).  The first generated audio frame therefore belongs
	 * to a fully loaded, stable stream. */
	sp.loaded = 1;
	UzeSidResetClock();
	play_state = PS_LOADED | PS_PLAYING;
	sp.redraw = 1;
	UzeSidLoadProgress(6);
	if(sp.entry.flags & UZESID_UZSD_FLAG_TRUNCATED){
		UMPrint(0, 2, PSTR("SPI RAM FULL; CAPTURE WILL LOOP"));
		WaitVsync(30);
	}
	return 0;
}

static u8 UzeSidLoadEntryByOrdinal(u16 ordinal)
{
	u32 entry_index;

	if(UzeSidReadLiveEntryByOrdinal(ordinal, &entry_index, &sp.entry) != 0)
		return 1;
	return UzeSidLoadEntryDirect(entry_index, &sp.entry, UZESID_SRC_CACHE);
}
static u8 UzeSidFindOrdinalByEntryIndex(u32 entry_index, u16 *ordinal_out)
{
	u32 i;
	u16 live = 0;

	if(!sp.usdc_ok)
		return 1;
	for(i = 0; i < sp.usdc.header.dir_entry_count; i++){
		if(UzesidUsdcReadEntry(&sp.usdc, i, &sp.entry) != 0)
			continue;
		if(sp.entry.state != UZESID_USDC_ENTRY_LIVE || sp.entry.type != UZESID_USDC_ENTRY_TYPE_USD)
			continue;
		if(i == entry_index){
			if(ordinal_out != 0)
				*ordinal_out = live;
			return 0;
		}
		live++;
	}
	return 1;
}
static void RestartSID(void)
{
	if(!sp.loaded)
		return;
	if(UzesidPlayerRestart(&sp.player) == 0){
		play_state = PS_LOADED | PS_PLAYING;
		UzeSidResetClock();
		sp.redraw = 1;
	}
}

static void UzeSidRenderAudioFrame(void)
{
	u8 *buf;
	/* Generate into shared staging SRAM first.  The player applies CIA ticks at
	 * their sub-frame sample positions, then the complete frame is published to
	 * whichever mixer bank is inactive after synthesis finishes. */
	(void)player_advance_pairs(&sp.player, sid_frame_staging,
		UZESID_AUDIO_PAIRS_PER_FRAME);
	buf = (mix_bank ? (u8 *)mix_buf : (u8 *)mix_buf + UZESID_FRAME_SAMPLES);
	memcpy(buf, sid_frame_staging, UZESID_FRAME_SAMPLES);
}

UZESID_NOINLINE static void RenderSID(void)
{
	static u8 timer_draw_div;
	u8 ff;
	u8 playing;
	u8 shell_redrawn = 0u;

	playing = (u8)(((play_state & PS_PAUSE) == 0u) &&
		((play_state & PS_PLAYING) != 0u) && sp.loaded);

	/* WaitVsync() returns at the start of the frame.  Produce audio immediately,
	 * before polling input or touching VRAM, so synthesis receives the complete
	 * frame budget.  Keep the normal 24-line display enabled throughout; the old
	 * temporary one-line mode was directly visible whenever work crossed vsync
	 * and was the source of the severe player-screen flicker. */
	if(playing){
		ff = sp.fast_forward;
		while(ff-- != 0u){
			(void)UzesidPlayerStepVideoFrame(&sp.player);
			UzeSidAdvanceClock();
		}
		UzeSidAdvanceClock();
		UzeSidRenderAudioFrame();
	}else{
		/* Match UzeMOD: the kernel keeps consuming the silent ring while idle. */
		SilenceBuffer();
	}

	/* Controls and GUI work are deliberately one frame behind audio.  That is
	 * imperceptible to input but prevents cursor/text work from stealing the
	 * mixer deadline. */
	PlayerInterface();

	if(!(play_state & PS_DRAWN) || sp.redraw){
		UzeSidRedrawShell();
		sp.redraw = 0;
		shell_redrawn = 1u;
	}else{
		u8 prev_scroll = g_song_title_scroll;
		UzeSidUpdateSongTitleScroll();
		if(g_song_title_scroll != prev_scroll)
			UzeSidPrintSongTitle((CONT_BAR_X / 8), (CONT_BAR_Y / 8) + 2, SONG_TITLE_VISIBLE_CHARS);
	}

	/* Text conversion and VRAM writes are unnecessary at 60 Hz.  Ten updates
	 * per second are visually smooth and leave more time for synthesis. */
	if(playing){
		if(shell_redrawn || ++timer_draw_div >= 6u){
			timer_draw_div = 0u;
			UMPrint(PTIME_X, PTIME_Y, PSTR("  :  :  "));
			PrintByte(PTIME_X + 8, PTIME_Y, ptime_frame, 1);
			PrintByte(PTIME_X + 5, PTIME_Y, ptime_sec, 1);
			PrintByte(PTIME_X + 2, PTIME_Y, ptime_min, 0);
			UMPrintChar(PTIME_X + 3, PTIME_Y, ':');
			UMPrintChar(PTIME_X + 6, PTIME_Y, ':');
		}
	}else{
		timer_draw_div = 0u;
	}
}


static void UzeSidBrowserInvertRow(u8 y, u8 invert)
{
	u8 x;
	u16 base = (u16)y * VRAM_TILES_H;
	for(x = 5; x < 26; x++){
		u8 t = vram[base + x];
		if(invert){
			if(t < 64u)
				vram[base + x] = (u8)(t + 64u);
		}else if(t >= 64u && t < 128u){
			vram[base + x] = (u8)(t - 64u);
		}
	}
}

static u32 UzeSidCacheBrowserReadRow(u8 row, char **title_out)
{
	u32 ofs = UZESID_CACHE_BROWSER_BASE +
		(u32)row * UZESID_CACHE_BROWSER_RECORD_SIZE;
	SpiRamReadInto((u8)(ofs >> 16), (u16)ofs,
		g_uzesid_workbuf, UZESID_CACHE_BROWSER_RECORD_SIZE);
	if(title_out != 0)
		*title_out = (char *)(g_uzesid_workbuf + 4);
	return UzesidRd32(g_uzesid_workbuf);
}

/* Read the contiguous USDC directory once for the requested visible page and
 * cache only ten {entry-index,title} records in SPI RAM.  The old browser
 * rescanned from directory entry zero for every row on every video frame;
 * with a populated database that could issue dozens of SD seeks per frame,
 * leaving only the first COMMANDO row visible and causing severe flicker. */
UZESID_NOINLINE static u8 UzeSidCacheBrowserLoadPage(u16 first_song, u16 *total_out, u8 count_all)
{
	u32 i;
	u32 dir_abs;
	u16 song_ordinal = 0;
	u8 rows = 0;
	UINT br;

	if(total_out != 0)
		*total_out = 0;
	if(!sp.usdc_ok)
		return 0;
	dir_abs = sp.usdc.base_offset + sp.usdc.header.dir_offset;
	if(pf_lseek(dir_abs) != FR_OK)
		return 0;

	for(i = 0; i < sp.usdc.header.dir_entry_count; i++){
		u8 state;
		u8 type;
		u16 subtune;
		if(pf_read(g_uzesid_workbuf, 128, &br) != FR_OK || br != 128u)
			break;
		while(rcv_spi() != 0xFF);
		state = g_uzesid_workbuf[0];
		type = g_uzesid_workbuf[1];
		subtune = UzesidRd16(g_uzesid_workbuf + 28);
		/* One USDC directory record exists for every subtune.  The browser is a
		 * file/song browser, so show only subtune zero for each SID instead of
		 * filling the first pages with repeated COMMANDO/Golden Axe titles. */
		if(state != UZESID_USDC_ENTRY_LIVE ||
			type != UZESID_USDC_ENTRY_TYPE_USD || subtune != 0u)
			continue;
		if(song_ordinal >= first_song && rows < UZESID_LIST_ROWS){
			u32 ofs = UZESID_CACHE_BROWSER_BASE +
				(u32)rows * UZESID_CACHE_BROWSER_RECORD_SIZE;
			/* Move the title before overwriting the beginning of the raw record. */
			memmove(g_uzesid_workbuf + 4, g_uzesid_workbuf + 44, 32);
			g_uzesid_workbuf[0] = (u8)i;
			g_uzesid_workbuf[1] = (u8)(i >> 8);
			g_uzesid_workbuf[2] = (u8)(i >> 16);
			g_uzesid_workbuf[3] = (u8)(i >> 24);
			g_uzesid_workbuf[35] = 0;
			SpiRamWriteFrom((u8)(ofs >> 16), (u16)ofs,
				g_uzesid_workbuf, UZESID_CACHE_BROWSER_RECORD_SIZE);
			rows++;
		}
		song_ordinal++;
		if(!count_all && rows >= UZESID_LIST_ROWS)
			break;
	}
	if(total_out != 0)
		*total_out = song_ordinal;
	return rows;
}

static void UzeSidCacheBrowserDrawPage(u16 foff, u16 total, u8 rows)
{
	u8 i;
	char *title;
	DrawWindow(4, 2, 22, SCREEN_TILES_V - 3,
		PSTR("Select SID"), PSTR("Cancel"), NULL);
	PrintInt(16, SCREEN_TILES_V - 1, (foff < total) ? (foff + 1) : total, 1);
	PrintInt(25, SCREEN_TILES_V - 1, total, 1);
	UMPrint(18, SCREEN_TILES_V - 1, PSTR("of"));
	SetTile(26, 2, TILE_WIN_SCRU);
	SetTile(26, SCREEN_TILES_V - 1, TILE_WIN_SCRD);
	for(i = 0; i < UZESID_LIST_ROWS; i++){
		if(i < rows){
			(void)UzeSidCacheBrowserReadRow(i, &title);
			UMPrintRamClip(5, 4 + i, title, UZESID_BROWSER_LIST_WIDTH);
		}else{
			UMPrintRamClip(5, 4 + i, "", UZESID_BROWSER_LIST_WIDTH);
		}
	}
}

UZESID_NOINLINE static void CacheSelectWindow(void)
{
	u16 foff = 0;
	u16 total = 0;
	u8 rows;
	u8 last_row = 0xffu;

	if(UzeSidEnsureDbOpen() != 0 || !sp.usdc_ok)
		return;

	SilenceBuffer();
	ClearVram();
	SetRenderingParameters(33, SCREEN_TILES_V * TILE_HEIGHT);
	UMPrint(0, 0, PSTR("INDEXING CACHED SIDS...        "));
	WaitVsync(1);
	/* The initial sequential directory pass both counts distinct SIDs and
	 * fills the first visible page.  Later page changes stop after ten rows. */
	rows = UzeSidCacheBrowserLoadPage(foff, &total, 1u);
	if(total == 0u){
		ReopenDbFile();
		SetRenderingParameters(33, 24);
		return;
	}
	ClearVram();
	UzeSidCacheBrowserDrawPage(foff, total, rows);

	while(1){
		u8 line;
		u8 row = 0xffu;
		u8 click = 0;
		WaitVsync(1);
		UpdateCursor(SCREEN_TILES_V * TILE_HEIGHT);
		line = sprites[0].y / 8;
		if(line >= 4u && line < (u8)(4u + rows))
			row = (u8)(line - 4u);
		if((pad & (BTN_Y | BTN_SL | BTN_SR | BTN_MOUSE_LEFT)) &&
			!(oldpad & (BTN_Y | BTN_SL | BTN_SR | BTN_MOUSE_LEFT)))
			click = 1;

		if(row != last_row){
			char *title;
			if(last_row != 0xffu)
				UzeSidBrowserInvertRow((u8)(4u + last_row), 0);
			last_row = row;
			UMPrint(0, 0, PSTR("Title:                         "));
			UMPrint(0, 1, PSTR("                              "));
			UMPrint(0, 2, PSTR("                              "));
			if(row != 0xffu){
				(void)UzeSidCacheBrowserReadRow(row, &title);
				UMPrintRamClip(7, 0, title, UZESID_BROWSER_PREVIEW_WIDTH);
				UzeSidBrowserInvertRow((u8)(4u + row), 1);
				PrintInt(16, SCREEN_TILES_V - 1,
					(u16)(foff + row + 1u), 1);
			}else{
				PrintInt(16, SCREEN_TILES_V - 1,
					(foff < total) ? (foff + 1u) : total, 1);
			}
		}

		if(click){
			if(ButtonHit(4, SCREEN_TILES_V - 1, 6, 1)){
				break;
			}else if(ButtonHit(26, 2, 1, 1)){
				u16 next = (foff >= UZESID_LIST_ROWS) ?
					(u16)(foff - UZESID_LIST_ROWS) : 0u;
				if(next != foff){
					foff = next;
					last_row = 0xffu;
					rows = UzeSidCacheBrowserLoadPage(foff, 0, 0u);
					UzeSidCacheBrowserDrawPage(foff, total, rows);
				}
			}else if(ButtonHit(26, SCREEN_TILES_V - 1, 1, 1)){
				if((u16)(foff + UZESID_LIST_ROWS) < total){
					foff = (u16)(foff + UZESID_LIST_ROWS);
					last_row = 0xffu;
					rows = UzeSidCacheBrowserLoadPage(foff, 0, 0u);
					UzeSidCacheBrowserDrawPage(foff, total, rows);
				}
			}else if(row != 0xffu && ButtonHit(5, 4, 20, UZESID_LIST_ROWS)){
				u32 entry_index = UzeSidCacheBrowserReadRow(row, 0);
				if(UzesidUsdcReadEntry(&sp.usdc, entry_index, &sp.entry) == 0)
					(void)UzeSidLoadEntryDirect(entry_index, &sp.entry, UZESID_SRC_CACHE);
				break;
			}
		}
	}

	ReopenDbFile();
	SetRenderingParameters(33, 24);
	play_state &= (u8)~PS_DRAWN;
	sp.redraw = 1;
	if(sprites[0].y > 20)
		sprites[0].y = 18;
	WaitVsync(1);
}

static u8 UzeSidLoadAdjacent(s8 dir)
{
	u16 total;
	u16 ord;
	if(!sp.loaded)
		return 1;
	/* This call is blocking, but the audio ISR continues consuming the last
	 * published mixer bank.  Clear both banks before any directory read, SD
	 * copy, raw load, or pre-emulation so old or partial audio cannot repeat
	 * while Previous/Next is still loading. */
	SilenceBuffer();
	if(sp.source == UZESID_SRC_TEMP){
		char path[32];
		s16 target;
		if(number_of_songs <= 1)
			return 1;
		if(dir < 0)
			target = (current_song == 0) ? (s16)(number_of_songs - 1) : (s16)(current_song - 1);
		else
			target = (current_song + 1 >= number_of_songs) ? 0 : (s16)(current_song + 1);
		SpiRamReadStringEntry(UZESID_SELECTED_PATH_OFS, path);
		if(path[0] == 0)
			return 1;
		UMPrint(0, 1, PSTR("Loading subtune...            "));
		WaitVsync(1);
		return UzeSidImportRawSid(path, target);
	}
	total = UzeSidGetLiveCount();
	if(total == 0)
		return 1;
	if(UzeSidFindOrdinalByEntryIndex(sp.current_index, &ord) != 0)
		return 1;
	if(dir < 0)
		ord = (ord == 0) ? (u16)(total - 1) : (u16)(ord - 1);
	else
		ord = (u16)((ord + 1 >= total) ? 0 : (ord + 1));
	return UzeSidLoadEntryByOrdinal(ord);
}

int main(void)
{
	FRESULT res = FR_OK;
	u8 i;
	u8 bank_count;
	u32 detected_ram;
	u8 moff;

	SetFontTilesIndex(0);
	SetTileTable((const char *)tile_data);
	SetSpritesTileTable((const char *)tile_data);
	ClearVram();
	DrawWindow(1, 1, 28, 10, NULL, NULL, NULL);
	do{
		WaitVsync(1);
	}while(DDRC != 255);
	LoadPreferences();
#if (ENABLE_MIXER == 1)
	SetMasterVolume(255);
#endif

	for(i = 0; i < 10; i++){
		res = pf_mount(&fs);
		if(res == FR_OK){
			UMPrint(3, 2, PSTR("Mounted SD Card"));
			break;
		}
		WaitVsync(30);
	}
	if(i >= 10){
		UMPrint(3, 2, PSTR("ERROR: SD Mount:"));
		PrintByte(20, 2, res, 0);
		goto MAIN_FAIL;
	}

	/* Match the known-good UzeMOD hardware sequence: do not clock the shared
	 * SPI bus before SpiRamInitGetSize() has configured PA4 as the RAM CS.
	 * Keep the SD card explicitly deselected while the RAM probe runs. */
	SD_DESELECT();
	bank_count = SpiRamInitGetSize();
	if(!bank_count){
		UMPrint(3, 3, PSTR("ERROR: No SPI RAM"));
		goto MAIN_FAIL;
	}
	detected_ram = (u32)bank_count * (64UL * 1024UL);
	g_detected_ram = detected_ram;
	moff = 21;
	if(bank_count > 1)
		moff++;
	if(bank_count > 15)
		moff++;
	PrintLong(moff, 3, detected_ram / 1024UL);
	UMPrintChar(moff + 1, 3, 'K');
	UMPrint(3, 3, PSTR("SPI RAM Detected:"));
	WaitVsync(60);

	SIDInit();
	SIDReset(0);
	memset(&sp, 0, sizeof(sp));
	sp.current_index = UZESID_INVALID_INDEX;
	UzesidUzeSidMakeSink(&sp.sink, &sp.sink_state);
	UzeSidClearLoadedState();
	UMPrint(3, 4, PSTR("DB deferred"));
	WaitVsync(20);

	sprites[0].tileIndex = TILE_CURSOR;
	sprites[0].flags = 0;
	sprites[0].x = CONT_BAR_X;
	sprites[0].y = CONT_BAR_Y + 4;
	SetRenderingParameters(33, 24);

	while(1){
		WaitVsync(1);
		RenderSID();
	}

MAIN_FAIL:
	WaitVsync(240);
	SoftReset();
	return 0;
}

static void PollPad(void)
{
	u16 pad2;
	InputDeviceHandler();
	oldpad = pad;
	pad = ReadJoypad(0);
	pad2 = ReadJoypad(1);
	/* A mouse may be connected to either controller port.  Keep port 1 as
	 * the normal joypad source, but merge port 2 mouse buttons just as UzeMOD
	 * does so either mouse can operate the interface. */
	if(pad2 & MOUSE_SIGNATURE)
		pad |= pad2 & (BTN_MOUSE_LEFT | BTN_MOUSE_RIGHT);
}

static void UzeSidClearLoadedState(void)
{
	sp.loaded = 0;
	sp.source = UZESID_SRC_NONE;
	sp.current_index = UZESID_INVALID_INDEX;
	memset(&sp.entry, 0, sizeof(sp.entry));
	UzeSidSetNoSongLoaded();
	play_state = 0;
	sp.redraw = 1;
}

static void InputDeviceHandler(void)
{
	u8 i;
	joypad1_status_lo = joypad2_status_lo = joypad1_status_hi = joypad2_status_hi = 0;

	JOYPAD_OUT_PORT |= _BV(JOYPAD_LATCH_PIN);
	for(i = 0; i < 8 + 1; i++)
		Wait200ns();
	JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_LATCH_PIN));

	for(i = 0; i < 16; i++){
		joypad1_status_lo >>= 1;
		joypad2_status_lo >>= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));
		if((JOYPAD_IN_PORT & (1 << JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_lo |= (1 << 15);
		if((JOYPAD_IN_PORT & (1 << JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_lo |= (1 << 15);
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j = 0; j < 33 + 1; j++)
			Wait200ns();
	}

	if(joypad1_status_lo == (BTN_START + BTN_SELECT + BTN_Y + BTN_B) || joypad2_status_lo == (BTN_START + BTN_SELECT + BTN_Y + BTN_B))
		SoftReset();

	/* Standard joypads end after 16 bits.  Only a Super Mouse/extended device
	 * needs the second 16 clocks.  Skipping them on the normal controller path
	 * halves the custom polling cost during playback. */
	if(((joypad1_status_lo | joypad2_status_lo) & MOUSE_SIGNATURE) == 0u)
		return;

	for(i = 0; i < 8 + 1; i++)
		Wait200ns();

	for(i = 0; i < 16; i++){
		joypad1_status_hi <<= 1;
		joypad2_status_hi <<= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));
		if((JOYPAD_IN_PORT & (1 << JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_hi |= 1;
		if((JOYPAD_IN_PORT & (1 << JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_hi |= 1;
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j = 0; j < 33 + 1; j++)
			Wait200ns();
	}
}

static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb)
{
	u8 x2, y2;
	SetTile(x + 0, y + 0, TILE_WIN_TLC);
	SetTile(x + w, y + 0, TILE_WIN_TRC);
	SetTile(x + 0, y + h, TILE_WIN_BLC);
	SetTile(x + w, y + h, TILE_WIN_BRC);
	for(y2 = y + 1; y2 < y + h; y2++){
		for(x2 = x + 1; x2 < x + w; x2++)
			SetTile(x2, y2, 0);
	}
	for(x2 = x + 1; x2 < x + w; x2++){
		SetTile(x2, y, TILE_WIN_TBAR);
		SetTile(x2, y + h, TILE_WIN_BBAR);
	}
	for(y2 = y + 1; y2 < y + h; y2++){
		SetTile(x, y2, TILE_WIN_LBAR);
		SetTile(x + w, y2, TILE_WIN_RBAR);
	}
	if(title != NULL)
		UMPrint(x + 1, y, title);
	if(lb != NULL)
		UMPrint(x + 1, y + h, lb);
	if(rb != NULL){
		u8 xo = x + w;
		for(u8 i = 0; i < 16; i++){
			if(pgm_read_byte(&rb[i]) == '\0')
				break;
			xo--;
		}
		UMPrint(xo, y + h, rb);
	}
}

static void UpdateCursor(u8 ylimit)
{
	u8 speed;
	PollPad();
	speed = (pad & BTN_SR) ? 1 : 2;

	if((pad & BTN_LEFT)){
		if(sprites[0].x < speed)
			sprites[0].x = 0;
		else
			sprites[0].x -= speed;
	}else if((pad & BTN_RIGHT)){
		if(sprites[0].x + speed > (SCREEN_TILES_H * TILE_WIDTH) - 9)
			sprites[0].x = (SCREEN_TILES_H * TILE_WIDTH) - 9;
		else
			sprites[0].x += speed;
	}

	if((pad & BTN_UP)){
		if(sprites[0].y < speed)
			sprites[0].y = 0;
		else
			sprites[0].y -= speed;
	}else if((pad & BTN_DOWN)){
		if(sprites[0].y + 8 + speed > ylimit + 3)
			sprites[0].y = ylimit - 8 + 3;
		else
			sprites[0].y += speed;
	}

	for(u8 i = 0; i < 2; i++){
		u16 p = ReadJoypad(i);
		u16 motion;
		s16 deltax, deltay;
		s16 mousex, mousey;
		const s16 mousexmax = (SCREEN_TILES_H * TILE_WIDTH) - 8;
		const s16 mouseymax = ylimit - 4;

		if(!(p & MOUSE_SIGNATURE))
			continue;

		motion = (i == 0) ? joypad1_status_hi : joypad2_status_hi;
		deltax = motion & 0x007f;
		deltay = (motion >> 8) & 0x007f;
		/* SNES mouse byte layout: X sign is bit 7 and Y sign is bit 15.
		 * The old UzeSID code had these two sign bits reversed. */
		if(motion & 0x0080)
			deltax = -deltax;
		if(motion & 0x8000)
			deltay = -deltay;

		mousex = (s16)sprites[0].x + deltax;
		mousey = (s16)sprites[0].y + deltay;
		if(mousex < 0)
			mousex = 0;
		else if(mousex > mousexmax)
			mousex = mousexmax;
		if(mousey < 0)
			mousey = 0;
		else if(mousey > mouseymax)
			mousey = mouseymax;

		sprites[0].x = (u8)mousex;
		sprites[0].y = (u8)mousey;
	}
}

UZESID_NOINLINE static void PlayerInterface(void)
{
	static u8 lastbtn = 255;
	u8 btn;
	u8 newclick = 0;
	u8 raw_shortcut;
	u8 cache_shortcut;

	UpdateCursor(24);
	raw_shortcut = ((pad & BTN_START) && !(oldpad & BTN_START));
	cache_shortcut = ((pad & BTN_SELECT) && !(oldpad & BTN_SELECT));
	if(sprites[0].y >= (CONT_BAR_Y + CONT_BTN_H) || sprites[0].x < CONT_BAR_X || sprites[0].x >= CONT_BAR_X + CONT_BAR_W)
		btn = 255;
	else
		btn = (u8)((sprites[0].x - CONT_BAR_X) / CONT_BTN_W);

	if((pad & (BTN_Y | BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y | BTN_MOUSE_LEFT))){
		lastbtn = btn;
		newclick = 1;
	}

	if(lastbtn != 255){
		u8 moff = (u8)(2 + (lastbtn * 2));
		u8 xoff = (u8)((CONT_BAR_X / 8) + (lastbtn * (CONT_BTN_W / 8)));
		u8 pressed = ((pad & (BTN_Y | BTN_MOUSE_LEFT)) && lastbtn != 7u) ? 0x28u : 0u;
		SetTile(xoff + 0, (CONT_BAR_Y / 8) + 0,
			(u8)(pgm_read_byte(&buttons_map[moff++]) + pressed));
		SetTile(xoff + 1, (CONT_BAR_Y / 8) + 0,
			(u8)(pgm_read_byte(&buttons_map[moff]) + pressed));
		moff += (CONT_BAR_W / TILE_WIDTH) - 1;
		SetTile(xoff + 0, (CONT_BAR_Y / 8) + 1,
			(u8)(pgm_read_byte(&buttons_map[moff++]) + pressed));
		SetTile(xoff + 1, (CONT_BAR_Y / 8) + 1,
			(u8)(pgm_read_byte(&buttons_map[moff]) + pressed));
		if(!(pad & (BTN_Y | BTN_MOUSE_LEFT)))
			lastbtn = 255;
	}

	sp.fast_forward = 0;
	if(raw_shortcut){
		btn = 6;
		newclick = 1;
	}else if(cache_shortcut){
		btn = 7;
		newclick = 1;
	}
	if(newclick){
		if(play_state & PS_LOADED){
			if(btn == 0){
				(void)UzeSidLoadAdjacent(-1);
			}else if(btn == 1){
				RestartSID();
			}else if(btn == 2){
				if(play_state & PS_PAUSE)
					play_state = (play_state & PS_DRAWN) | PS_LOADED | PS_PLAYING;
				else
					play_state = (play_state & PS_DRAWN) | PS_LOADED | PS_PAUSE;
			}else if(btn == 3 || btn == 4){
				play_state = PS_LOADED | PS_DRAWN | PS_PLAYING;
			}else if(btn == 5){
				(void)UzeSidLoadAdjacent(1);
			}
		}
		if(btn == 6 || btn == 7){
			if(btn == 6 && g_detected_ram < UZESID_RAW_REQUIRED_RAM){
				UzeSidTitleSet("Raw SID needs 128K SPI RAM");
			}else{
				WaitVsync(1);
				if(btn == 6)
					RawSidSelectWindow();
				else
					CacheSelectWindow();
			}
			pad = oldpad = 0x0fff;
			WaitVsync(1);
			play_state &= (u8)~PS_DRAWN;
			sp.redraw = 1;
		}else if(btn == 8){
			if(masterVolume){
				masterVolume--;
				update_sid_gains(sid);
			}
		}else if(btn == 9){
			if(masterVolume < 64){
				masterVolume++;
				update_sid_gains(sid);
			}
		}else if(btn == 10){
			do {
				DDRC--;
			} while(UZESID_COLOR_MASK_INVALID(DDRC));
		}else if(btn == 11){
			do {
				DDRC++;
			} while(UZESID_COLOR_MASK_INVALID(DDRC));
		}else if(btn == 12){
			SavePreferences();
		}
	}
	if(lastbtn == 4 && (pad & (BTN_Y | BTN_MOUSE_LEFT)))
		sp.fast_forward = 2;
}


static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h)
{
	if(sprites[0].x < (x << 3) || sprites[0].x >= (x << 3) + (w << 3) || sprites[0].y < (y << 3) || sprites[0].y >= (y << 3) + (h << 3))
		return 0;
	return 1;
}

static void LoadPreferences(void)
{
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;
	if(EepromReadBlock(ebs.id, &ebs) == 0){
		DDRC = ebs.data[UZESID_PREFS_COLOR_INDEX];
		if(ebs.data[UZESID_PREFS_MARKER_INDEX] == UZESID_PREFS_MARKER &&
		   ebs.data[UZESID_PREFS_VOLUME_INDEX] <= 64)
			masterVolume = ebs.data[UZESID_PREFS_VOLUME_INDEX];
		else
			masterVolume = 64;
	}else{
		DDRC = DEFAULT_COLOR_MASK;
		masterVolume = 64;
	}
}

static void SavePreferences(void)
{
	struct EepromBlockStruct ebs;
	ebs.id = UZENET_EEPROM_ID1;
	if(EepromReadBlock(ebs.id, &ebs)){
		for(u8 i = 0; i < 30; i++)
			ebs.data[i] = 0;
	}
	ebs.data[UZESID_PREFS_MARKER_INDEX] = UZESID_PREFS_MARKER;
	ebs.data[UZESID_PREFS_VOLUME_INDEX] = masterVolume;
	ebs.data[UZESID_PREFS_COLOR_INDEX] = DDRC;
	EepromWriteBlock(&ebs);
}

static void UMPrintChar(u8 x, u8 y, char c)
{
	u8 uc = (u8)c;
	if(uc >= 'a' && uc <= 'z')
		uc -= 32;
	if(uc < 32 || uc > 95)
		uc = '?';
	SetTile(x, y, (u8)(uc - 32));
}

static void UMPrint(u8 x, u8 y, const char *s)
{
	u8 soff = 0;
	do{
		char c = pgm_read_byte(&s[soff++]);
		if(c == '\0')
			break;
		UMPrintChar(x++, y, c);
	}while(1);
}

static void UMPrintRamClip(u8 x, u8 y, const char *s, u8 width)
{
	u8 i = 0;
	while(i < width && *s){
		UMPrintChar(x + i, y, *s++);
		i++;
	}
	while(i < width){
		SetTile(x + i, y, 0);
		i++;
	}
}

/* ---- raw SID import helpers (root directory only, first pass) ---- */

static void SpiRamWriteStringEntry(u32 pos, const char *s)
{
	u8 i = 0;
	/* Use the bounded block API instead of opening a hand-written sequential
	 * transaction immediately after a Petit FatFs directory operation.  This
	 * makes the first browser scan deterministic on the shared SD/SPI-RAM bus. */
	while(i < 31u && s[i] != 0){
		g_uzesid_workbuf[i] = (u8)s[i];
		i++;
	}
	g_uzesid_workbuf[i++] = 0;
	while(i < 32u)
		g_uzesid_workbuf[i++] = 0;
	SpiRamWriteFrom((u8)(pos >> 16), (u16)pos, g_uzesid_workbuf, 32);
}

static void SpiRamReadStringEntry(u32 pos, char *dst)
{
	SpiRamReadInto((u8)(pos >> 16), (u16)(pos & 0xffffu), dst, 32);
	dst[31] = 0;
}

UZESID_NOINLINE static u16 LoadRootSidList(void)
{
	DIR dir;
	FILINFO fno;
	FRESULT res;
	u16 total = 0;
	res = pf_opendir(&dir, "/");
	if(res == FR_OK){
		while(total < UZESID_CARDLIST_MAX){
			res = pf_readdir(&dir, &fno);
			while(rcv_spi() != 0xFF);
			if(res != FR_OK || fno.fname[0] == 0)
				break;
			if(fno.fattrib & AM_DIR)
				continue;
			if(!HasExt(fno.fname, ".SID"))
				continue;
			/* A same-named embedded SID is represented by the DB row appended
			 * below, avoiding duplicate card/cache entries in the merged list. */
			{
				u8 subtunes;
				if(UzeSidFindPrebuilt(fno.fname, g_uzesid_workbuf, &subtunes))
					continue;
			}
			SpiRamWriteStringEntry(UZESID_RAWLIST_BASE + (u32)total * 32UL, fno.fname);
			total++;
		}
	}
	/* The generated prebuilt table is a compact filename -> MD5 index for every
	 * SID baked into the USDC database. Append names not already present on the
	 * card so cached-only songs remain selectable without their source .SID. */
#if UZESID_PREBUILT_COUNT > 0
	{
		u8 n;
		for(n = 0; n < (u8)UZESID_PREBUILT_COUNT && total < UZESID_RAWLIST_MAX; n++){
			u8 k;
			char *name = (char *)(g_uzesid_workbuf + 32u);
			for(k = 0; k < 31u; k++){
				name[k] = (char)pgm_read_byte(&g_prebuilt_sids[n].name[k]);
				if(name[k] == 0)
					break;
			}
			name[31] = 0;
			SpiRamWriteStringEntry(UZESID_RAWLIST_BASE + (u32)total * 32UL, name);
			total++;
		}
	}
#endif
	return total;
}

UZESID_NOINLINE static u8 ReadSidMetaTitle(const char *path, char *title)
{
	u8 header[64];
	UINT br;
	if(pf_open(path) != FR_OK)
		return 1;
	if(pf_read(header, 64, &br) != FR_OK || br < 0x36)
		return 2;
	while(rcv_spi() != 0xFF);
	if(header[0] != 'P' || header[1] != 'S' || header[2] != 'I' || header[3] != 'D')
		return 3;
	memcpy(title, header + 0x16, 31);
	title[31] = 0;
	{
		u8 i;
		for(i = 0; i < 32; i++){
			u8 c = (u8)title[i];
			if(c == 0)
				break;
			if(!((c >= 32 && c <= 95) || (c >= 'a' && c <= 'z')))
				title[i] = ' ';
		}
		while(i != 0 && title[i - 1] == ' ')
			title[--i] = 0;
	}
	return 0;
}

static u8 ReopenDbFile(void)
{
	FRESULT res;

	/* The database is opened lazily at startup.  A prebuilt filename can be
	 * selected before LIDX/USDC has ever been decoded.  In that case perform
	 * the full open rather than merely selecting the UZE file and poisoning
	 * db_open with uninitialized directory state. */
	if(!sp.lidx_ok && !sp.usdc_ok)
		return UzeSidOpenDb();

	res = pf_open(UZESID_ROM_FILENAME);
	if(res != FR_OK){
		sp.db_open = 0;
		sp.lidx_ok = 0;
		sp.usdc_ok = 0;
		return 1;
	}
	UzesidPffInitReader(&sp.reader, &sp.pff_ctx);
	sp.db_open = 1;
	return 0;
}

UZESID_NOINLINE static u8 UzeSidImportRawSid(const char *path, s16 requested_song)
{
	u32 entry_index;
	/* Open bundled prebuilt streams before loading the PSID.  Preserve raw-SID
	 * context so Previous/Next selects another cached subtune of the same file
	 * instead of moving through the global cache directory. */
	{
		u8 subtune_count;
		s16 target = (requested_song < 0) ? 0 : requested_song;
		if(target >= 0 && UzeSidFindPrebuilt(path, sp.entry.md5, &subtune_count) &&
			(u16)target < subtune_count){
			u8 find_rc;
			/* A browser title preview may have made a raw SID the active Petit
			 * FatFs file while the decoded DB offsets remain cached in sp. Always
			 * reopen UZESID.UZE before following a prebuilt-only selection. */
			if(ReopenDbFile() == 0 && sp.usdc_ok){
				find_rc = UzesidUsdcFindEntry(&sp.usdc, sp.entry.md5, (u16)target,
					&sp.entry, &entry_index);
				if(find_rc == 0){
					number_of_songs = subtune_count;
					current_song = target;
					UMPrint(0, 0, PSTR("LOADING PREBUILT CACHE...     "));
					if(UzeSidLoadEntryDirect(entry_index, &sp.entry, UZESID_SRC_TEMP) == 0)
						return 0;
				}
			}
		}
	}
#if UZESID_ENABLE_CACHE_WRITE
	if(SP_SAVE_STATE != UZESID_SAVE_IDLE)
		return 5;
#endif
	u8 rc;
	u8 need_md5 = 0;

	/* Open the database headers before the SID.  Hash when an existing stream
	 * could match or when writeback is enabled, since the MD5/subtune pair is
	 * the persistent cache key. */
	if(UzeSidEnsureDbOpen() == 0 && sp.usdc_ok && sp.usdc.header.live_entries != 0u)
		need_md5 = 1;
#if UZESID_ENABLE_CACHE_WRITE
	need_md5 = 1;
#endif

	/* Reuse sp.entry as the 128-byte PSID header/read scratch instead of
	 * reserving that buffer on the AVR stack. */
	UzeSidLoadProgress(0);
	if(!LoadPSIDFilePff(path, need_md5 ? sp.entry.md5 : 0, &sp.entry, requested_song)){
		if(g_uzesid_psid_error == UZESID_PSID_ERROR_TOO_LARGE)
			return 3;
		if(g_uzesid_psid_error == UZESID_PSID_ERROR_TRUNCATED)
			return 4;
		return 1;
	}
	if(!need_md5)
		memset(sp.entry.md5, 0, sizeof(sp.entry.md5));

	if(need_md5){
		UzeSidLoadProgress(4);
		/* Hashing reopens the SID. Petit FatFs has one active file, so reopen
		 * UZESID.UZE before using its decoded directory offsets. */
		if(ReopenDbFile() == 0 && sp.usdc_ok){
			rc = UzesidUsdcFindEntry(&sp.usdc, sp.entry.md5, (u16)current_song, &sp.entry, &entry_index);
			if(rc == 0 && UzeSidLoadEntryDirect(entry_index, &sp.entry, UZESID_SRC_TEMP) == 0)
				return 0;
		}
	}

	/* No persistent cache entry exists. Pre-emulate once into the second
	 * SPI-RAM bank, then use the lightweight register-stream player. */
	rc = UzeSidLoadTempCurrent();
	return (rc == 0) ? 0 : 2;
}

UZESID_NOINLINE static void RawSidSelectWindow(void)
{
	u16 total = LoadRootSidList();
	u16 foff = 0;
	u16 last_ord = 0xFFFFu;
	u8 import_error = 0;
	char name[32];

	if(total == 0)
		return;

	SilenceBuffer();
	ClearVram();
	SetRenderingParameters(33, SCREEN_TILES_V * TILE_HEIGHT);
	while(1){
		u8 line;
		u8 click = 0;
		u8 i;
		WaitVsync(1);
		UpdateCursor(SCREEN_TILES_V * TILE_HEIGHT);
		line = sprites[0].y / 8;
		if((pad & (BTN_Y | BTN_SL | BTN_SR | BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y | BTN_SL | BTN_SR | BTN_MOUSE_LEFT)))
			click = 1;
		DrawWindow(4, 2, 22, SCREEN_TILES_V - 3, PSTR("Open SID"), PSTR("Cancel"), NULL);
		PrintInt(16, SCREEN_TILES_V - 1, (foff < total) ? (foff + 1) : total, 1);
		PrintInt(25, SCREEN_TILES_V - 1, total, 1);
		UMPrint(18, SCREEN_TILES_V - 1, PSTR("of"));
		SetTile(26, 2, TILE_WIN_SCRU);
		SetTile(26, SCREEN_TILES_V - 1, TILE_WIN_SCRD);
		{
			u16 cur_ord = 0xFFFFu;
			for(i = 0; i < UZESID_LIST_ROWS; i++){
				u16 ord = (u16)(foff + i);
				if(ord >= total) break;
				SpiRamReadStringEntry(UZESID_RAWLIST_BASE + (u32)ord * 32UL, name);
				UMPrintRamClip(5, 4 + i, name, UZESID_BROWSER_LIST_WIDTH);
				if(line == i + 4){
					u8 k;
					cur_ord = ord;
					for(k = 5; k < 26; k++) vram[(line * VRAM_TILES_H) + k] += 64;
				}
			}
			if(cur_ord != last_ord){
				last_ord = cur_ord;
				UMPrint(0, 0, PSTR("Title:                         "));
				UMPrint(0, 1, PSTR("                              "));
				UMPrint(0, 2, PSTR("                              "));
				if(cur_ord != 0xFFFFu){
					SpiRamReadStringEntry(UZESID_RAWLIST_BASE + (u32)cur_ord * 32UL, name);
					if(ReadSidMetaTitle(name, sp.entry.title) == 0)
						UMPrintRamClip(7, 0, sp.entry.title, UZESID_BROWSER_PREVIEW_WIDTH);
					else
						UMPrintRamClip(7, 0, name, UZESID_BROWSER_PREVIEW_WIDTH);
				}
			}
		}
		if(import_error == 1)
			UMPrint(0, 1, PSTR("SID LOAD FAILED               "));
		else if(import_error == 2)
			UMPrint(0, 1, PSTR("SID IMPORT FAILED             "));
		else if(import_error == 3)
			UMPrint(0, 1, PSTR("SID DATA EXCEEDS 64K RAM      "));
		else if(import_error == 4)
			UMPrint(0, 1, PSTR("SID FILE IS TRUNCATED         "));
		if(click){
			if(ButtonHit(4, SCREEN_TILES_V - 1, 6, 1)){
				break;
			}else if(ButtonHit(26, 2, 1, 1)){
				if(foff >= UZESID_LIST_ROWS) foff = (u16)(foff - UZESID_LIST_ROWS); else foff = 0;
			}else if(ButtonHit(26, SCREEN_TILES_V - 1, 1, 1)){
				if((u16)(foff + UZESID_LIST_ROWS) < total) foff = (u16)(foff + UZESID_LIST_ROWS);
			}else if(ButtonHit(5, 4, 20, UZESID_LIST_ROWS)){
				u16 ord = (u16)(foff + (line - 4));
				if(ord < total){
					SpiRamReadStringEntry(UZESID_RAWLIST_BASE + (u32)ord * 32UL, name);
					SpiRamWriteStringEntry(UZESID_SELECTED_PATH_OFS, name);
					UMPrint(0, 1, PSTR("Loading SID...                "));
					WaitVsync(1);
					import_error = UzeSidImportRawSid(name, -1);
					if(import_error == 0) break;
				}
			}
		}
	}
	ReopenDbFile();
	SetRenderingParameters(33, 24);
	play_state &= (u8)~PS_DRAWN;
	sp.redraw = 1;
	if(sprites[0].y > 20) sprites[0].y = 18;
	WaitVsync(1);
}
