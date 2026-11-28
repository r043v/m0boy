#ifndef M0BOY_GP32_HLE_H
#define M0BOY_GP32_HLE_H

#include "s3c2400.h"

/* Initialize a firmware-free GP32 and load a validated application.
   Mount SmartMedia and load EEPROM on the resulting SoC after boot. */
const char *gp32_hle_boot(S3c2400 *soc, const uint8_t *data, size_t size,
                         Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks,
                         S3c2400Video video);

/* The card directory of an application installed from a package, which a plain
   image no longer tells. */
void gp32_hle_set_directory(S3c2400 *soc, const char *directory);

#endif
