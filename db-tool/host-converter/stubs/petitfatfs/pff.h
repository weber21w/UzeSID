#ifndef PFF_H
#define PFF_H
#include "diskio.h"
typedef struct { uint8_t dummy[32]; DWORD fsize; } FATFS;
typedef struct { uint8_t dummy[32]; } DIR;
typedef struct { char fname[32]; BYTE fattrib; DWORD fsize; } FILINFO;
typedef uint8_t FRESULT;
#define FR_OK 0
#define AM_DIR 0x10
#define FA_READ 1
FRESULT pf_mount(FATFS*);
FRESULT pf_open(const char*);
FRESULT pf_read(void*, UINT, UINT*);
FRESULT pf_write(const void*, UINT, UINT*);
FRESULT pf_lseek(DWORD);
FRESULT pf_opendir(DIR*, const char*);
FRESULT pf_readdir(DIR*, FILINFO*);
#endif
