#ifndef SPIRAM_H
#define SPIRAM_H
#include <stdint.h>
void SpiRamWrite(uint8_t bank, uint16_t addr, uint8_t value);
uint8_t SpiRamRead(uint8_t bank, uint16_t addr);
void SpiRamWriteFrom(uint8_t bank, uint16_t addr, const void *src, uint16_t len);
void SpiRamReadInto(uint8_t bank, uint16_t addr, void *dst, uint16_t len);
uint8_t SpiRamInitGetSize(void);
void SpiRamSeqReadStart(uint8_t bank, uint16_t addr);
uint8_t SpiRamSeqReadU8(void);
void SpiRamSeqReadEnd(void);
void SpiRamSeqWriteStart(uint8_t bank, uint16_t addr);
void SpiRamSeqWriteU8(uint8_t value);
void SpiRamSeqWriteEnd(void);
#endif
