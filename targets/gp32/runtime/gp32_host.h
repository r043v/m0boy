#ifndef M0BOY_GP32_HOST_H
#define M0BOY_GP32_HOST_H

#include "s3c2400.h"

typedef struct {
    const char *card_path, *eeprom_path, *exec_path;
    uint8_t smcid[16], pduid[16];
    bool has_smcid, has_pduid, show_ids;
    uint8_t *image;
    size_t image_size;
    char *directory;
    char workspace[256], snapshot[288];
    char package_card[1024];
    bool readonly, hle;
} Gp32Host;

bool gp32_host_identify(const char *path);
bool gp32_host_options(Gp32Host *host, int argc, char **argv);
bool gp32_host_boot(Gp32Host *host, S3c2400 *soc, uint8_t **bios,
                    Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks, S3c2400Video video);
void gp32_host_print_ids(const Gp32Host *host, const S3c2400 *soc);
bool gp32_host_mount(Gp32Host *host, S3c2400 *soc);
/* Run up to `steps` instructions, returning early on a new frame.
   False once the machine cannot continue. */
bool gp32_host_run(const Gp32Host *host, S3c2400 *soc, uint64_t steps, uint64_t *exceptions);
bool gp32_host_step(const Gp32Host *host, S3c2400 *soc, uint64_t *exceptions);
/* Replace a file atomically. */
bool gp32_host_save_file(const char *path, const uint8_t *data, size_t size);
bool gp32_host_close(Gp32Host *host, const S3c2400 *soc);

#endif
