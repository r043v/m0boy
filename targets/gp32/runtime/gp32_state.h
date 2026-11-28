#ifndef M0BOY_GP32_STATE_H
#define M0BOY_GP32_STATE_H

#include "gp32_host.h"

#include <stddef.h>

/* Save states of a running machine: the SoC, its memories and the SmartMedia
   contents. A state loads into a machine booted the same way (same
   application, same card size, same build layout). Call these between two
   s3c2400_run calls. Everything that belongs to the host (callbacks, buffers,
   pacing settings) stays as in the live machine. */

/* Identifies the application a state belongs to. */
uint64_t gp32_state_identity(const char *path);

/* The same state in memory: a malloc'd buffer, NULL on failure. */
uint8_t *gp32_state_pack(const S3c2400 *soc, const Gp32Host *host, uint64_t identity, size_t *size);
bool gp32_state_unpack(S3c2400 *soc, Gp32Host *host, uint64_t identity, const uint8_t *data,
                       size_t size);
/* No state of this machine is larger. */
size_t gp32_state_limit(const Gp32Host *host);

bool gp32_state_save(const S3c2400 *soc, const Gp32Host *host, uint64_t identity, const char *path);
/* False leaves the machine untouched. */
bool gp32_state_load(S3c2400 *soc, Gp32Host *host, uint64_t identity, const char *path);

/* Back to `boot`, a copy of the machine taken right after boot. The card and
   the EEPROM keep what has been written to them. */
bool gp32_state_restart(S3c2400 *soc, const S3c2400 *boot);

#endif
