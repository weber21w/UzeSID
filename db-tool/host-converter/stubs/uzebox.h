#ifndef UZEBOX_H
#define UZEBOX_H
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint8_t BYTE;
#define SCREEN_TILES_H 32
#define SCREEN_TILES_V 15
#define TILE_WIDTH 8
#define TILE_HEIGHT 8
#define RAM_TILES_COUNT 1
#define VRAM_TILES_H 32
#define BTN_B 0x0001
#define BTN_Y 0x0002
#define BTN_SELECT 0x0004
#define BTN_START 0x0008
#define BTN_UP 0x0010
#define BTN_DOWN 0x0020
#define BTN_LEFT 0x0040
#define BTN_RIGHT 0x0080
#define BTN_A 0x0100
#define BTN_X 0x0200
#define BTN_SL 0x0400
#define BTN_SR 0x0800
#define BTN_MOUSE_LEFT 0x1000
#define BTN_MOUSE_RIGHT 0x2000
#define MOUSE_SIGNATURE 0x8000
#define EEPROM_BLOCK_SIZE 30
typedef struct { uint8_t x,y,tileIndex,flags; } Sprite;
struct EepromBlockStruct { uint8_t id; uint8_t data[EEPROM_BLOCK_SIZE]; };
extern Sprite sprites[];
void SetRenderingParameters(uint8_t,uint8_t);
void WaitVsync(uint8_t);
void ClearVram(void);
void DrawMap(uint8_t,uint8_t,const char*);
void SetTile(uint8_t,uint8_t,uint8_t);
void SetSpriteVisibility(uint8_t,uint8_t);
void SetTileTable(const char*);
void SetSpritesTileTable(const char*);
void SetFontTilesIndex(uint8_t);
void SoftReset(void);
uint16_t ReadJoypad(uint8_t);
uint8_t GetVsyncFlag(void);
uint8_t EepromReadBlock(uint8_t, void*);
void EepromWriteBlock(const void*);
void PrintByte(uint8_t,uint8_t,uint8_t,uint8_t);
void PrintLong(uint8_t,uint8_t,uint32_t);
void PrintInt(uint8_t,uint8_t,int,uint8_t);
extern volatile uint8_t JOYPAD_OUT_PORT, JOYPAD_IN_PORT;
#define JOYPAD_LATCH_PIN 0
#define JOYPAD_CLOCK_PIN 1
#define JOYPAD_DATA1_PIN 2
#define JOYPAD_DATA2_PIN 3
#endif
