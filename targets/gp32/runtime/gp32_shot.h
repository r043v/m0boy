#ifndef M0BOY_GP32_SHOT_H
#define M0BOY_GP32_SHOT_H

#include "gp32_host.h"

/* A shot is a PNG of the game screen that also carries the whole machine state,
   SmartMedia contents included: a private chunk, "gpSt", holds the size of a
   save state (gp32_state.h, 8 bytes little endian) and its zlib stream. Any
   image viewer shows it and any build of the same program restarts from it. */

#define GP32_SHOT_WIDTH 320u
#define GP32_SHOT_HEIGHT 240u

/* Identifies the program as loaded, whatever file or package carries it. */
uint64_t gp32_shot_program(const char *exec_path);
/* The screen as displayed: landscape RGB with the panel response curve. */
void gp32_shot_screen(S3c2400 *soc, uint8_t *rgb);

/* Call these between two s3c2400_run calls (after s3c2400_sync for a machine
   advanced with s3c2400_step_batched). */
bool gp32_shot_save(S3c2400 *soc, const Gp32Host *host, uint64_t program, const char *path);
/* False leaves the machine untouched. */
bool gp32_shot_load(S3c2400 *soc, Gp32Host *host, uint64_t program, const char *path);

#endif
