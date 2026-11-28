#ifndef M0BOY_GP32_VCARD_H
#define M0BOY_GP32_VCARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A host directory presented as a 32 MiB SmartMedia card, for one machine.

   The card is generated while it is read, with the layout of make_card.py: CIS,
   logical block addresses and ECC, one FAT16 partition, and the directory's
   files in consecutive clusters.  Nothing is converted beforehand.  What the
   guest writes goes to a swap file; gp32_vcard_sync() then writes the files it
   changed back into the directory.

   Names must fit 8.3: others are reported and left off the card.  An optional
   CARD.UID file of 1024 bytes is presented as the card's identity header. */

typedef void (*Gp32VcardReport)(const char *message, const char *path);

/* Returns the size to give gp32_smc_attach(), 0 on failure.  GAME and GPMM are
   created in the directory when missing. */
size_t gp32_vcard_open(const char *folder, const char *swap_path, Gp32VcardReport report);

/* A byte of the raw card, valid to the end of its 512-byte sector and until the
   next call: the shape of gp32_card_bytes(). */
uint8_t *gp32_vcard_bytes(size_t offset, bool write);
void gp32_vcard_flush(void);

/* Grows with every write of the guest.  Call gp32_vcard_sync() once it has
   stopped growing for a while. */
uint32_t gp32_vcard_writes(void);

/* Brings the directory up to date with the guest's card.  A file the guest has
   not closed yet is reported and left as it was.  Returns false, and
   reports why, when the card does not hold a complete file system (the
   directory is then left as it was) or when a host file cannot be written. */
bool gp32_vcard_sync(void);

void gp32_vcard_close(void);

#endif
