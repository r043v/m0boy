#ifndef M0BOY_GP32_SMC_H
#define M0BOY_GP32_SMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *image;
    unsigned offset, pages, page_size, data_size, block_pages, row_cycles;
    unsigned row, column, address_bytes, area;
    uint8_t id[3], mode, program[528], uid[16];
    bool writable, dirty, failed, uid_mode, uid_override;
} Gp32Smc;

/* The backing image is caller-owned. Use a private copy for each fork that
   can write to the card. The raw representation includes NAND spare bytes. */
#ifdef M0_GP32_PAGED_CARD
/* The platform's side of a paged card. */
uint8_t *gp32_card_bytes(size_t offset, bool write);
void gp32_card_flush(void);
#endif

bool gp32_smc_attach(Gp32Smc *card, uint8_t *image, size_t size, bool writable);
bool gp32_smc_set_uid(Gp32Smc *card, const uint8_t uid[16]);
const uint8_t *gp32_smc_uid(const Gp32Smc *card);
void gp32_smc_command(Gp32Smc *card, uint8_t command, bool write_enable);
void gp32_smc_address(Gp32Smc *card, uint8_t value);
void gp32_smc_write(Gp32Smc *card, uint8_t value);
uint8_t gp32_smc_read(Gp32Smc *card, bool write_enable);

#endif
