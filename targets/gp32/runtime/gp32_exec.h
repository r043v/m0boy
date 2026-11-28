#ifndef M0BOY_GP32_EXEC_H
#define M0BOY_GP32_EXEC_H

#include "s3c2400.h"

bool gp32_exec_parse_id(const char hex[32], uint8_t id[16]);

/* Decode the ID field of a 64-byte GPS/GPM record. The record's filename
   distinguishes a console identity (.GPS) from a SmartMedia identity (.GPM). */
bool gp32_exec_read_identity(const uint8_t record[64], uint8_t id[16]);

typedef enum {
    GP32_EXEC_ID_UNKNOWN, GP32_EXEC_ID_NONE, GP32_EXEC_ID_SMC, GP32_EXEC_ID_MACHINE
} Gp32ExecIdentity;

/* Reports the identity requested by an installed GXE descriptor. */
Gp32ExecIdentity gp32_exec_identity(const uint8_t *data, size_t size);

/* The card directory a ZPK package installs into, which its game builds file
   paths from. Empty for other formats. */
void gp32_exec_directory(const uint8_t *data, size_t size, char directory[256]);

/* A resource shipped with a package. `path` is relative to the SmartMedia root,
   uses '/' separators and never leaves it. `data` is NULL for a directory. */
typedef bool (*Gp32ExecResource)(void *context, const char *path, const uint8_t *data, size_t size);

/* Unpack the resource archive of a ZPK package, in archive order. Reports no
   entry for other formats or for a package without resources. */
const char *gp32_exec_resources(const uint8_t *data, size_t size, Gp32ExecResource resource, void *context);

/* Load a decoded application into SDRAM and report the decoded size. */
const char *gp32_exec_load_ram(S3c2400 *soc, const uint8_t *data, size_t size, size_t limit, size_t *loaded_size);
#ifdef M0_GP32_PAGED_RAM
/* The same for a GXB image already in guest memory, at or above its load
   address. */
const char *gp32_exec_load_guest(S3c2400 *soc, uint32_t source, size_t size, size_t limit, size_t *loaded_size);
#endif

/* Returns an allocated firmware image with the application in its boot slot.
   Keep it alive for the lifetime of the machine. */
const char *gp32_exec_boot_image(const uint8_t *bios, const uint8_t *data, size_t size,
                                uint8_t **image, size_t *image_size);

#endif
