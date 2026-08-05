/*
 *  Uzebox Kernel
 *  Copyright (C) 2008-2009 Alec Bourque
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Uzebox is a reserved trade mark
*/

/*
 * Audio mixer than mixes the sound channels to a circular buffer during VSYNC.
 *
 */

#include <avr/io.h>
#include <defines.h>

.global update_sound_buffer
.global update_sound_buffer_2
.global update_sound_buffer_fast
.global process_music
.global mix_pos
.global mix_buf
.global mix_bank
.global sound_enabled
.global update_sound

//Public variables
.global mixer

.section .bss

mix_buf: 	  .space MIX_BUF_SIZE
mix_pos:	  .space 2
mix_bank: 	  .space 1 ;0=first half,1=second half
mix_block:	  .space 1

sound_enabled:.space 1

.section .text

;**********************
; Mix sound and process music track
; NOTE: registers r18-r27 are already saved by the caller
;***********************
process_music:
	;Flip mix bank & set target bank adress for mixing
	lds r0,mix_bank
	tst r0
	brne set_hi_bank
	ldi XL,lo8(mix_buf)
	ldi XH,hi8(mix_buf)
	rjmp end_set_bank
set_hi_bank:
	ldi XL,lo8(mix_buf+MIX_BANK_SIZE)
	ldi XH,hi8(mix_buf+MIX_BANK_SIZE)
end_set_bank:

	ldi r18,1
	eor	r0,r18
	sts mix_bank,r0
	
	ldi r18,2
	sts mix_block,r18	

	ret


;**********************************
; Optimized sound output (cannot be called during double rate sync)
; NO MIDI
; Destroys: Z,r16,r17
; Cycles: 24
;**********************************
update_sound_buffer_fast:
	lds ZL,mix_pos
	lds ZH,mix_pos+1
			
	ld r16,Z+		;load next sample
	;subi r16,128	;convert to unsigned
	nop
	sts _SFR_MEM_ADDR(OCR2A),r16 ;output sound byte

	;compare+wrap=8 cycles fixed
	ldi r16,hi8(MIX_BUF_SIZE+mix_buf)
	cpi ZL,lo8(MIX_BUF_SIZE+mix_buf)
	cpc ZH,r16
	;12

	ldi r16,lo8(mix_buf)
	ldi r17,hi8(mix_buf)
	brlo .+2
	movw ZL,r16

	sts mix_pos,ZL
	sts mix_pos+1,ZH		

	ret ;20+4=24


;************************
; Regular sound output used during hblanks.
; Handles MIDI.
; In: ZL = video phase (1=pre-eq/post-eq, 2=hsync)
; Destroys: ZH
; Cycles: VSYNC = 68
;         HSYNC = 135
;***********************
update_sound:
	push r16
	push r17
	push r18
	push ZL

	lds ZL,mix_pos
	lds ZH,mix_pos+1
			
	ld r16,Z+
	sts _SFR_MEM_ADDR(OCR2A),r16 ;output sound byte

	;compare+wrap=8 cycles fixed
	ldi r16,hi8(MIX_BUF_SIZE+mix_buf)
	cpi ZL,lo8(MIX_BUF_SIZE+mix_buf)
	cpc ZH,r16

	ldi r16,lo8(mix_buf)
	ldi r17,hi8(mix_buf)

	brlo .+2
	movw ZL,r16

	sts mix_pos,ZL
	sts mix_pos+1,ZH	


#if UART_RX_BUFFER == 1
	;read MIDI-in data (27 cycles)
	ldi ZL,lo8(uart_rx_buf)
	ldi ZH,hi8(uart_rx_buf)
	lds r16,uart_rx_buf_end
	
	clr r17
	add ZL,r16
	adc ZH,r17

	lds r17,_SFR_MEM_ADDR(UCSR0A)	
	lds r18,_SFR_MEM_ADDR(UDR0)

	st Z,r18
	
	sbrc r17,RXC0
	inc r16
	andi r16,(UART_RX_BUFFER_SIZE-1) ;wrap
	sts uart_rx_buf_end,r16
	
	rjmp .
	rjmp .
	rjmp .
#else
	//alignment cycles
	ldi ZL,8
	dec ZL
	brne .-4
#endif

	pop ZL
	pop r18
	pop r17
	pop r16

	;*** Video sync update ***
	sbrc ZL,0								;pre-eq/post-eq sync
	sbi _SFR_IO_ADDR(SYNC_PORT),SYNC_PIN	;TCNT1=0xAC
	sbrs ZL,0								
	rjmp .+2
	ret

	ldi ZH,20
	dec ZH
	brne .-4
	rjmp .

	;*** Video sync update ***
	sbrc ZL,1								;hsync
	sbi _SFR_IO_ADDR(SYNC_PORT),SYNC_PIN	;TCNT1=0xF0
	sbrs ZL,1								
	rjmp .

	ret 

