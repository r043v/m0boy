#ifndef M0BOY_GP32_AKA_CARD_H
#define M0BOY_GP32_AKA_CARD_H

#include <stddef.h>
#include <stdint.h>

#include "gp32_payload.h" /* which of the two cards this firmware has */

/* GP32_AKA_CARD_FOLDER: a folder of the SD card is the SmartMedia card
   (gp32_vcard.h), and `name` is its path.  Otherwise `name` is the 8.3 name (11
   characters, space padded) of a card image, in the root or one folder below.
   Returns the card's size, 0 on failure (no card, no such folder or file, or a
   file in too many fragments). */
size_t gp32_aka_card_open(const char *name);

/* Call between two runs of the machine: writes what the program saved back
   into the folder.  Nothing to do for an image. */
void gp32_aka_card_service(uint32_t microseconds);
/* The same at once, before the firmware stops. */
void gp32_aka_card_close(void);

#endif
