#ifndef AVR_IO_H
#define AVR_IO_H
#include <stdint.h>
extern volatile uint8_t PORTD, DDRC, OCR2A, SREG;
#define _BV(x) (1u<<(x))
#endif
