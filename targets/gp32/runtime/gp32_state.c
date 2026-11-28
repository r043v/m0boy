#include "gp32_state.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* File: header, the S3c2400 structure with its host pointers cleared, then the
   card cut at the data and spare areas of its pages. Each chunk is either
   CHUNK_RAW and its bytes, or CHUNK_FILL and the byte it repeats. */

#define STATE_MAGIC "M0G32ST1"
#define PAGE_DATA 512u
#define PAGE_SPARE 16u

enum { CHUNK_RAW, CHUNK_FILL };

static size_t chunk_length(size_t at, size_t image)
{
    size_t length = at % (PAGE_DATA + PAGE_SPARE) ? PAGE_SPARE : PAGE_DATA;
    return image - at < length ? image - at : length;
}

typedef struct {
    char magic[8];
    uint64_t identity, image_size, soc_size, payload_size, checksum;
} Header;

static uint64_t fnv(uint64_t hash, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ data[i]) * UINT64_C(0x100000001b3);
    return hash;
}

#define FNV_START UINT64_C(0xcbf29ce484222325)

uint64_t gp32_state_identity(const char *path)
{
    uint64_t hash = FNV_START;
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file)
        return hash;
    uint8_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) != 0)
        hash = fnv(hash, buffer, count);
    fclose(file);
    return hash;
}

/* Replace every host owned member of `state` by the one of `live`. */
static void keep_host(S3c2400 *state, const S3c2400 *live)
{
    state->bus = live->bus;
    state->cpu.config = live->cpu.config;
    state->video = live->video;
    state->lcd_capture = live->lcd_capture;
    state->irq_handler = live->irq_handler;
    state->audio = live->audio;
    state->cycles_per_instruction = live->cycles_per_instruction;
    state->smc.image = live->smc.image;
    state->smc.writable = live->smc.writable;
}

/* Two chunks per page, the last page possibly partial. */
static size_t card_chunks(size_t image)
{
    return 2 * (image / (PAGE_DATA + PAGE_SPARE) + 1);
}

static size_t card_size(const S3c2400 *soc, const Gp32Host *host)
{
    return soc->smc.image ? host->image_size : 0;
}

uint8_t *gp32_state_pack(const S3c2400 *soc, const Gp32Host *host, uint64_t identity, size_t *size)
{
    size_t image = card_size(soc, host);
    uint8_t *data = malloc(sizeof(Header) + sizeof(*soc) + card_chunks(image) + image);
    S3c2400 *copy = malloc(sizeof(*copy));
    S3c2400 *clear = calloc(1, sizeof(*clear));
    if (data && copy && clear) {
        memcpy(copy, soc, sizeof(*copy));
        keep_host(copy, clear);
        uint8_t *out = data + sizeof(Header);
        memcpy(out, copy, sizeof(*copy));
        out += sizeof(*copy);
        for (size_t at = 0, length; at < image; at += length) {
            length = chunk_length(at, image);
            const uint8_t *chunk = host->image + at;
            bool fill = length == 1 || !memcmp(chunk, chunk + 1, length - 1);
            *out++ = fill ? CHUNK_FILL : CHUNK_RAW;
            memcpy(out, chunk, fill ? 1 : length);
            out += fill ? 1 : length;
        }
        Header header = {.identity = identity, .image_size = image, .soc_size = sizeof(*soc),
                         .payload_size = (uint64_t)(out - data) - sizeof(Header)};
        memcpy(header.magic, STATE_MAGIC, sizeof(header.magic));
        header.checksum = fnv(FNV_START, data + sizeof(Header), (size_t)header.payload_size);
        memcpy(data, &header, sizeof(header));
        *size = (size_t)(out - data);
    } else {
        free(data);
        data = NULL;
    }
    free(clear);
    free(copy);
    return data;
}

size_t gp32_state_limit(const Gp32Host *host)
{
    return sizeof(Header) + sizeof(S3c2400) + card_chunks(host->image_size) + host->image_size;
}

bool gp32_state_unpack(S3c2400 *soc, Gp32Host *host, uint64_t identity, const uint8_t *file,
                       size_t file_size)
{
    size_t image = card_size(soc, host);
    Header header;
    uint8_t *card = NULL;
    S3c2400 *state = NULL;
    /* The first format ends where the members added later begin. */
    const size_t first = offsetof(S3c2400, late);
    if (file_size < sizeof(header))
        return false;
    memcpy(&header, file, sizeof(header));
    const uint8_t *data = file + sizeof(header);
    size_t size = file_size - sizeof(header);
    bool loaded = !memcmp(header.magic, STATE_MAGIC, sizeof(header.magic)) &&
                  header.identity == identity && header.image_size == image &&
                  (header.soc_size == sizeof(*soc) || header.soc_size == first) &&
                  header.payload_size == size && size >= header.soc_size &&
                  size <= header.soc_size + card_chunks(image) + image &&
                  fnv(FNV_START, data, size) == header.checksum;
    loaded = loaded && (state = malloc(sizeof(*state))) != NULL &&
             (!image || (card = malloc(image)) != NULL);
    if (loaded) {
        size_t stored_soc = (size_t)header.soc_size;
        memset(state, 0, sizeof(*state));
        memcpy(state, data, stored_soc);
        if (stored_soc == first)
            s3c2400_upgrade_state(state, !soc->bus.boot_rom);
        const uint8_t *in = data + stored_soc, *end = data + size;
        for (size_t at = 0, length; loaded && at < image; at += length) {
            length = chunk_length(at, image);
            bool fill = in < end && *in == CHUNK_FILL;
            size_t stored = fill ? 1 : length;
            loaded = in < end && *in <= CHUNK_FILL && (size_t)(end - in - 1) >= stored;
            if (!loaded)
                break;
            ++in;
            if (fill)
                memset(card + at, *in, length);
            else
                memcpy(card + at, in, length);
            in += stored;
        }
        loaded = loaded && in == end;
    }
    if (loaded) {
        keep_host(state, soc);
        /* What the state brings back must reach the host files again. */
        state->smc.dirty = soc->smc.dirty || (image && memcmp(host->image, card, image));
        state->eeprom_dirty = soc->eeprom_dirty ||
                              memcmp(soc->eeprom, state->eeprom, sizeof(state->eeprom));
        if (image)
            memcpy(host->image, card, image);
        memcpy(soc, state, sizeof(*soc));
    }
    free(state);
    free(card);
    return loaded;
}

bool gp32_state_save(const S3c2400 *soc, const Gp32Host *host, uint64_t identity, const char *path)
{
    size_t size;
    uint8_t *data = gp32_state_pack(soc, host, identity, &size);
    bool saved = data && gp32_host_save_file(path, data, size);
    free(data);
    return saved;
}

bool gp32_state_load(S3c2400 *soc, Gp32Host *host, uint64_t identity, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    size_t limit = gp32_state_limit(host);
    uint8_t *data = malloc(limit + 1);
    size_t size = data ? fread(data, 1, limit + 1, file) : 0;
    bool loaded = data && size <= limit && !ferror(file);
    fclose(file);
    loaded = loaded && gp32_state_unpack(soc, host, identity, data, size);
    free(data);
    return loaded;
}

bool gp32_state_restart(S3c2400 *soc, const S3c2400 *boot)
{
    S3c2400 *state = malloc(sizeof(*state));
    if (!state)
        return false;
    memcpy(state, boot, sizeof(*state));
    keep_host(state, soc);
    memcpy(state->eeprom, soc->eeprom, sizeof(state->eeprom));
    state->eeprom_dirty = soc->eeprom_dirty;
    state->smc.dirty = soc->smc.dirty;
    memcpy(soc, state, sizeof(*soc));
    free(state);
    return true;
}
