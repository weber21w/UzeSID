#ifndef AVR_PGMSPACE_H
#define AVR_PGMSPACE_H
#include <stdint.h>
#define PROGMEM
#define PSTR(x) (x)
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define pgm_read_word(p) (*(const uint16_t *)(p))
#define pgm_read_dword(p) (*(const uint32_t *)(p))
#endif
