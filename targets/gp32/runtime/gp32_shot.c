#include "gp32_shot.h"

#include "gp32_exec.h"
#include "gp32_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

uint64_t gp32_shot_program(const char *exec_path)
{
    FILE *file = exec_path ? fopen(exec_path, "rb") : NULL;
    if (!file)
        return gp32_state_identity(exec_path);
    const size_t limit = 64u << 20;
    uint8_t *data = NULL;
    long length = fseek(file, 0, SEEK_END) ? -1 : ftell(file);
    size_t size = length > 0 && (size_t)length <= limit && !fseek(file, 0, SEEK_SET)
                      ? (size_t)length : 0;
    bool read = size && (data = malloc(size)) != NULL && fread(data, 1, size, file) == size;
    fclose(file);
    S3c2400 *soc = read ? calloc(1, sizeof(*soc)) : NULL;
    size_t loaded = 0;
    uint64_t hash = 0;
    if (soc && !gp32_exec_load_ram(soc, data, size, GP32_RAM_SIZE, &loaded) && loaded) {
        hash = UINT64_C(0xcbf29ce484222325);
        for (size_t i = 0; i < loaded; ++i)
            hash = (hash ^ soc->ram[i]) * UINT64_C(0x100000001b3);
    }
    free(soc);
    free(data);
    /* A raw firmware is not an application: name it by its file. */
    return hash ? hash : gp32_state_identity(exec_path);
}

void gp32_shot_screen(S3c2400 *soc, uint8_t *rgb)
{
    uint8_t curve[256];
    for (unsigned i = 0; i < 256; ++i)
        curve[i] = (uint8_t)(235.0 * pow(i / 255.0, 1.22) + 0.5);
    uint32_t *panel = malloc(GP32_LCD_WIDTH * GP32_LCD_HEIGHT * sizeof(*panel));
    if (!panel) {
        memset(rgb, 0, GP32_SHOT_WIDTH * GP32_SHOT_HEIGHT * 3);
        return;
    }
    s3c2400_screen(soc, panel);
    /* The panel scans columns, bottom to top. */
    for (unsigned y = 0; y < GP32_SHOT_HEIGHT; ++y)
        for (unsigned x = 0; x < GP32_SHOT_WIDTH; ++x) {
            uint32_t color = panel[x * GP32_LCD_WIDTH + (GP32_LCD_WIDTH - 1 - y)];
            *rgb++ = curve[(color >> 16) & 255];
            *rgb++ = curve[(color >> 8) & 255];
            *rgb++ = curve[color & 255];
        }
    free(panel);
}

static void store32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (uint8_t)(value >> (24 - 8 * i));
}

static uint32_t load32(const uint8_t *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

/* `out` holds the length, the type and `size` bytes of data already in place. */
static uint8_t *close_chunk(uint8_t *out, const char *type, size_t size)
{
    store32(out, (uint32_t)size);
    memcpy(out + 4, type, 4);
    store32(out + 8 + size, (uint32_t)crc32(0, out + 4, (uInt)(size + 4)));
    return out + 12 + size;
}

bool gp32_shot_save(S3c2400 *soc, const Gp32Host *host, uint64_t program, const char *path)
{
    enum { ROW = GP32_SHOT_WIDTH * 3 + 1, PIXELS = ROW * GP32_SHOT_HEIGHT };
    size_t state_size = 0;
    uint8_t *state = gp32_state_pack(soc, host, program, &state_size);
    uint8_t *rgb = malloc(GP32_SHOT_WIDTH * GP32_SHOT_HEIGHT * 3);
    uint8_t *rows = malloc(PIXELS);
    uLong image_bound = compressBound(PIXELS), state_bound = compressBound((uLong)state_size);
    uint8_t *png = state ? malloc(sizeof(signature) + 25 + 12 + image_bound + 20 + state_bound + 12)
                         : NULL;
    bool saved = false;
    if (state && rgb && rows && png) {
        gp32_shot_screen(soc, rgb);
        for (unsigned y = 0; y < GP32_SHOT_HEIGHT; ++y) {
            rows[y * ROW] = 0;
            memcpy(rows + y * ROW + 1, rgb + y * (ROW - 1), ROW - 1);
        }
        uint8_t *out = png;
        memcpy(out, signature, sizeof(signature));
        out += sizeof(signature);
        store32(out + 8, GP32_SHOT_WIDTH);
        store32(out + 12, GP32_SHOT_HEIGHT);
        memcpy(out + 16, "\10\2\0\0\0", 5);
        out = close_chunk(out, "IHDR", 13);
        uLongf image = image_bound, packed = state_bound;
        if (compress2(out + 8, &image, rows, PIXELS, 6) == Z_OK) {
            out = close_chunk(out, "IDAT", image);
            for (unsigned i = 0; i < 8; ++i)
                out[8 + i] = (uint8_t)((uint64_t)state_size >> (8 * i));
            /* Level 3: a third of a second for a state, within a tenth of the
               smallest size. */
            if (compress2(out + 16, &packed, state, (uLong)state_size, 3) == Z_OK) {
                out = close_chunk(out, "gpSt", 8 + packed);
                out = close_chunk(out, "IEND", 0);
                saved = gp32_host_save_file(path, png, (size_t)(out - png));
            }
        }
    }
    free(png);
    free(rows);
    free(rgb);
    free(state);
    return saved;
}

bool gp32_shot_load(S3c2400 *soc, Gp32Host *host, uint64_t program, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    uint8_t head[8];
    bool found = fread(head, 1, 8, file) == 8 && !memcmp(head, signature, 8);
    uint8_t *chunk = NULL;
    size_t length = 0;
    const size_t limit = gp32_state_limit(host);
    while (found && fread(head, 1, 8, file) == 8) {
        length = load32(head);
        if (!memcmp(head + 4, "gpSt", 4)) {
            uint8_t crc[4];
            found = length > 8 && length <= compressBound((uLong)limit) + 8 &&
                    (chunk = malloc(length + 4)) != NULL &&
                    fread(chunk + 4, 1, length, file) == length && fread(crc, 1, 4, file) == 4;
            if (found) {
                memcpy(chunk, head + 4, 4);
                found = crc32(0, chunk, (uInt)(length + 4)) == load32(crc);
            }
            break;
        }
        found = memcmp(head + 4, "IEND", 4) && length < 0x7fffffffu &&
                !fseek(file, (long)length + 4, SEEK_CUR);
    }
    fclose(file);
    bool loaded = false;
    if (found && chunk) {
        uint64_t stated = 0;
        for (unsigned i = 0; i < 8; ++i)
            stated |= (uint64_t)chunk[4 + i] << (8 * i);
        uint8_t *state = stated && stated <= limit ? malloc((size_t)stated) : NULL;
        uLongf size = (uLongf)stated;
        loaded = state && uncompress(state, &size, chunk + 12, (uLong)(length - 8)) == Z_OK &&
                 size == stated && gp32_state_unpack(soc, host, program, state, size);
        free(state);
    }
    free(chunk);
    return loaded;
}
