// On a desktop "program memory" is ordinary memory.
#ifndef EMU_PGMSPACE_H
#define EMU_PGMSPACE_H

#include <string.h>

#define PROGMEM
#define PGM_P const char *
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#define pgm_read_dword(addr) (*(const unsigned long *)(addr))
#define pgm_read_pointer(addr) (*(void *const *)(addr))
#define pgm_read_ptr(addr) (*(void *const *)(addr))
#define strcpy_P strcpy
#define strlen_P strlen

#endif
