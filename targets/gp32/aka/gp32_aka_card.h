#ifndef M0BOY_GP32_AKA_CARD_H
#define M0BOY_GP32_AKA_CARD_H

#include <stddef.h>

/* Find the card image on the SD card by its 8.3 name (11 characters, space
   padded), in the root or one folder below.  Returns its size, 0 on failure
   (no card, no file, or a file in too many fragments). */
size_t gp32_aka_card_open(const char *name83);

#endif
