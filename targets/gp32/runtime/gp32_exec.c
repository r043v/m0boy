#include "gp32_exec.h"

#include <stdlib.h>
#include <string.h>

enum {
    SLOT = 0x8000, HEADER = 28,
    ZPK_GXE = 0x175, ZPK_CHUNK_HEADER = 20
};

static uint32_t word(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

bool gp32_exec_parse_id(const char hex[32], uint8_t id[16])
{
    uint8_t decoded[16] = {0};
    for (unsigned i = 0; i < 32; ++i) {
        unsigned c = (unsigned char)hex[i];
        unsigned nibble;
        if (c >= '0' && c <= '9')
            nibble = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'f')
            nibble = (c | 32) - 'a' + 10;
        else
            return false;
        decoded[i / 2] = decoded[i / 2] * 16 + nibble;
    }
    memcpy(id, decoded, sizeof(decoded));
    return true;
}

bool gp32_exec_read_identity(const uint8_t record[64], uint8_t id[16])
{
    static const char mask[] = "TASTERSCHOICE";
    char hex[32];
    for (unsigned i = 0; i < sizeof(hex); ++i)
        hex[i] = record[32 + i] ^ mask[(32 + i) % (sizeof(mask) - 1)];
    return gp32_exec_parse_id(hex, id);
}

static bool valid_gxb(const uint8_t *data, size_t size)
{
    const uint32_t base = S3C2400_SDRAM_BASE, top = base + GP32_RAM_SIZE;
    if (size < HEADER || size > GP32_RAM_SIZE || word(data + 4) != base)
        return false;
    uint32_t branch = word(data);
    uint32_t target = 8u + ((branch & 0xffffffu) << 2);
    uint32_t ro_end = word(data + 8), bss = word(data + 12), end = word(data + 16);
    return (branch & 0xff000000u) == 0xea000000u &&
           target >= HEADER && target <= size - 4 &&
           ro_end >= base && ro_end - base <= size &&
           bss >= ro_end && end >= bss && end <= top;
}

Gp32ExecIdentity gp32_exec_identity(const uint8_t *data, size_t size)
{
    if (valid_gxb(data, size) ||
        (size >= 0x464 && !memcmp(data, "fxe ", 4)) ||
        (size == GP32_BIOS_SIZE && (word(data) & 0xff000000u) == 0xea000000u))
        return GP32_EXEC_ID_NONE;
    if (size >= ZPK_GXE + ZPK_CHUNK_HEADER && !memcmp(data, "ZPK ", 4) &&
        !memcmp(data + ZPK_GXE, "GXE ", 4)) {
        size_t gxe_size = word(data + ZPK_GXE + 16);
        if (gxe_size > size - ZPK_GXE - ZPK_CHUNK_HEADER)
            return GP32_EXEC_ID_UNKNOWN;
        return gp32_exec_identity(data + ZPK_GXE + ZPK_CHUNK_HEADER, gxe_size);
    }
    if (size < 0x74 || word(data) != 0x6c)
        return GP32_EXEC_ID_UNKNOWN;
    /* The menu seeks relative to the end of the fixed GXE header, then
       reads two records (44 and 4 bytes) before the encryption descriptor. */
    size_t relative = word(data + 0x70);
    if (relative > size - 0x74 || size - 0x74 - relative < 52)
        return GP32_EXEC_ID_UNKNOWN;
    size_t at = 0x74 + relative;
    uint32_t descriptor = word(data + at + 48);
    unsigned mode = descriptor & 255u, header = descriptor >> 16;
    if (mode > 2 || (mode && (!header || header > size - at - 52)))
        return GP32_EXEC_ID_UNKNOWN;
    return mode == 1 ? GP32_EXEC_ID_SMC : mode == 2 ? GP32_EXEC_ID_MACHINE : GP32_EXEC_ID_NONE;
}

/* This GamePark distribution profile stores the XOR table in the GXE
   metadata and places its matching GXC section immediately afterwards.

   The offsets below move with the padding in front of the encryption
   descriptor. A profile may also keep the head of the image in the GXE,
   in front of the table, and only ship the remainder in the GXC. */
static const char *decode_zpk(const uint8_t *data, size_t size, uint8_t **decoded,
                              size_t *payload_size)
{
    enum {
        RELATIVE_FIELD = 0x70, BASE_RELATIVE = 0x410, STRIDE_FIELD = 0x560,
        KEY_FIELD = 0x5dc, KEY_SIZE = 96, SCRAMBLED = 256,
        PROFILE_FIELD = 44, PROFILE_PLAIN = 0x228, PROFILE_SPLIT = 0x328
    };
    const size_t gxe_at = ZPK_GXE + ZPK_CHUNK_HEADER;
    if (size < gxe_at || memcmp(data + ZPK_GXE, "GXE ", 4))
        return "Unsupported ZPK package layout";
    const uint8_t *gxe = data + gxe_at;
    size_t gxe_size = word(data + ZPK_GXE + 16);
    if (gxe_size > size - gxe_at || gxe_size < KEY_FIELD + KEY_SIZE)
        return "Unsupported ZPK package layout";
    size_t relative = word(gxe + RELATIVE_FIELD);
    if (relative < BASE_RELATIVE || relative > gxe_size - 0x74 - PROFILE_FIELD - 4)
        return "Unsupported ZPK package layout";
    size_t shift = relative - BASE_RELATIVE;
    uint32_t profile = word(gxe + 0x74 + relative + PROFILE_FIELD);
    if (profile != PROFILE_PLAIN && profile != PROFILE_SPLIT)
        return "Unsupported ZPK encryption profile";
    size_t prefix = profile - PROFILE_PLAIN;
    /* The key follows the prefix and has to fit in the chunk. */
    if (shift > gxe_size - KEY_FIELD - KEY_SIZE - prefix)
        return "Unsupported ZPK package layout";
    const uint8_t *key = gxe + KEY_FIELD + shift + prefix;
    size_t stride = word(gxe + STRIDE_FIELD + shift);
    size_t gxc = gxe_at + gxe_size;
    if (size - gxc < 2 * ZPK_CHUNK_HEADER)
        return "Unsupported ZPK package layout";
    size_t content = gxc + 2 * ZPK_CHUNK_HEADER;
    /* At least one shipped package has a blank section tag. */
    if (memcmp(data + gxc, "GXC ", 4) && memcmp(data + gxc, "\0\0\0\0", 4))
        return "Unsupported ZPK package layout";
    size_t section = word(data + gxc + 16);
    size_t length = word(data + gxc + ZPK_CHUNK_HEADER);
    if (stride < SCRAMBLED || stride > GP32_RAM_SIZE || section < ZPK_CHUNK_HEADER ||
        section - ZPK_CHUNK_HEADER != length || length > GP32_RAM_SIZE - prefix ||
        length > size - content || prefix + length < HEADER)
        return "Invalid ZPK executable section";
    size_t total = prefix + length;
    uint8_t *out = malloc(total);
    if (!out)
        return "Out of memory decoding ZPK";
    memcpy(out, gxe + KEY_FIELD + shift, prefix);
    memcpy(out + prefix, data + content, length);
    for (size_t offset = 0; offset < total; offset += stride)
        for (size_t i = 0; i < SCRAMBLED && i < total - offset; ++i)
            out[offset + i] ^= key[i % KEY_SIZE];
    *decoded = out;
    *payload_size = total;
    return NULL;
}

void gp32_exec_directory(const uint8_t *data, size_t size, char directory[256])
{
    enum { FIELD = 0x6d };
    directory[0] = 0;
    if (size < ZPK_GXE || memcmp(data, "ZPK ", 4) || memcmp(data + FIELD, "gp:\\", 4) ||
        !memchr(data + FIELD, 0, 255))
        return;
    strcpy(directory, (const char *)data + FIELD);
}

/* MS-DOS COMPRESS (SZDD): LZSS over a 4 KiB window primed with spaces. */
static bool expand_szdd(const uint8_t *data, size_t size, uint8_t *output, size_t length)
{
    uint8_t window[4096];
    memset(window, ' ', sizeof(window));
    unsigned cursor = sizeof(window) - 16, flags = 1;
    size_t in = 0, out = 0;
    while (out < length) {
        if (flags == 1) {
            if (in == size)
                return false;
            flags = data[in++] | 0x100;
        }
        if (flags & 1) {
            if (in == size)
                return false;
            output[out++] = window[cursor] = data[in++];
            cursor = (cursor + 1) & 4095;
        } else {
            if (size - in < 2)
                return false;
            unsigned from = data[in] | (data[in + 1] & 0xf0u) << 4;
            unsigned count = (data[in + 1] & 15u) + 3;
            in += 2;
            for (unsigned i = 0; i < count && out < length; ++i) {
                output[out++] = window[cursor] = window[(from + i) & 4095];
                cursor = (cursor + 1) & 4095;
            }
        }
        flags >>= 1;
    }
    return true;
}

/* Read a length-prefixed, NUL-terminated string. */
static const char *archive_string(const uint8_t *data, size_t size, size_t *at)
{
    if (*at >= size)
        return NULL;
    size_t length = data[*at];
    if (!length || length >= size - *at || data[*at + length])
        return NULL;
    const char *text = (const char *)data + *at + 1;
    *at += length + 1;
    return text;
}

/* Append '\'-separated components to a relative path, refusing any that could
   escape the card root. */
static bool append_path(char *path, size_t capacity, const char *text)
{
    size_t used = strlen(path);
    while (*text) {
        size_t length = strcspn(text, "\\");
        if (length) {
            if (!strncmp(text, ".", length) || (length == 2 && !strncmp(text, "..", 2)) ||
                used + length + 2 > capacity)
                return false;
            if (used)
                path[used++] = '/';
            for (size_t i = 0; i < length; ++i) {
                unsigned char c = text[i];
                if (c < 32 || c == '/' || c == ':')
                    return false;
                path[used++] = c;
            }
            path[used] = 0;
        }
        text += length + (text[length] != 0);
    }
    return true;
}

const char *gp32_exec_resources(const uint8_t *data, size_t size, Gp32ExecResource resource, void *context)
{
    static const uint8_t magic[8] = {'S', 'Z', 'D', 'D', 0x88, 0xf0, 0x27, 0x33};
    enum { SZDD_HEADER = 14, ARCHIVE_LIMIT = 64 << 20, DIRECTORY = 0x30, REGULAR = 0x31, END = 0x32 };
    if (size < ZPK_GXE + ZPK_CHUNK_HEADER || memcmp(data, "ZPK ", 4))
        return NULL;
    size_t at = ZPK_GXE;
    for (unsigned chunk = 0; chunk < 2; ++chunk) { /* GXE, then GXC */
        if (size - at < ZPK_CHUNK_HEADER || word(data + at + 16) > size - at - ZPK_CHUNK_HEADER)
            return "Unsupported ZPK package layout";
        at += ZPK_CHUNK_HEADER + word(data + at + 16);
    }
    if (size - at < SZDD_HEADER || memcmp(data + at, magic, sizeof(magic)))
        return NULL;
    size_t length = word(data + at + 10);
    if (!length || length > ARCHIVE_LIMIT)
        return "Invalid ZPK resource archive";
    uint8_t *archive = malloc(length);
    if (!archive)
        return "Out of memory unpacking ZPK resources";
    const char *error = "Invalid ZPK resource archive";
    if (!expand_szdd(data + at + SZDD_HEADER, size - at - SZDD_HEADER, archive, length))
        goto done;
    for (size_t i = 0; i < length;) {
        unsigned kind = archive[i++];
        if (kind == END)
            break;
        if (kind != DIRECTORY && kind != REGULAR)
            goto done;
        const char *directory = archive_string(archive, length, &i);
        char path[512] = "";
        if (!directory || (directory[0] | 32) != 'g' || (directory[1] | 32) != 'p' ||
            directory[2] != ':' || !append_path(path, sizeof(path), directory + 3))
            goto done;
        const uint8_t *content = NULL;
        size_t bytes = 0;
        if (kind == REGULAR) {
            const char *name = archive_string(archive, length, &i);
            if (!name || strchr(name, '\\') || !*name || !append_path(path, sizeof(path), name) ||
                length - i < 4 || word(archive + i) > length - i - 4)
                goto done;
            bytes = word(archive + i);
            content = archive + i + 4;
            i += 4 + bytes;
        }
        if (*path && !resource(context, path, content, bytes)) {
            error = "Unable to store a ZPK resource";
            goto done;
        }
    }
    error = NULL;
done:
    free(archive);
    return error;
}

static const char *decode_application(const uint8_t *data, size_t size,
                                      const uint8_t **payload_out, size_t *size_out,
                                      uint8_t **allocation)
{
    const uint8_t *payload = data;
    uint8_t *decoded = NULL;
    if (size >= 4 && !memcmp(data, "ZPK ", 4)) {
        const char *error = decode_zpk(data, size, &decoded, &size);
        if (error)
            return error;
        payload = decoded;
    } else if (size >= 4 && !memcmp(data, "fxe ", 4)) {
        if (size < 12 || word(data + 8) < 0x454 || word(data + 8) > size - 12)
            return "Invalid FXE header";
        size_t key_at = (size_t)word(data + 8) + 16;
        if (key_at > size)
            return "Truncated FXE header";
        size_t ro = word(data + key_at - 8), rw = word(data + key_at - 4);
        if (!rw || ro > GP32_RAM_SIZE || rw > GP32_RAM_SIZE - ro ||
            rw > size - key_at || ro > size - key_at - rw)
            return "Invalid FXE section sizes";
        decoded = malloc(ro + rw);
        if (!decoded)
            return "Out of memory decoding FXE";
        for (size_t i = 0; i < rw; ++i)
            decoded[ro + i] = data[key_at + i] ^ 0xff;
        for (size_t i = 0; i < ro; ++i)
            decoded[i] = data[key_at + rw + i] ^ decoded[ro + i % rw];
        payload = decoded;
        size = ro + rw;
    }

    if (!valid_gxb(payload, size)) {
        free(decoded);
        Gp32ExecIdentity identity = gp32_exec_identity(data, size);
        if (identity == GP32_EXEC_ID_SMC || identity == GP32_EXEC_ID_MACHINE)
            return "Install the GXE/GXC pair on SmartMedia and select it in the GP32 BIOS GAME menu";
        return "Expected a GP32 FXE, GXB or supported ZPK application";
    }
    *payload_out = payload;
    *size_out = size;
    *allocation = decoded;
    return NULL;
}

/* Place the sections of a loaded GXB image.  `header` still holds the
   source's header; the source is host bytes, or guest memory above the load
   address when `payload` is null. */
static const char *install(S3c2400 *soc, const uint8_t *header, const uint8_t *payload,
                           uint32_t source, size_t size, size_t limit, size_t *loaded_size)
{
    if (size > limit || word(header + 16) - S3C2400_SDRAM_BASE > limit)
        return "GP32 executable sections exceed the HLE memory layout";
    uint32_t ro_end = word(header + 8) - S3C2400_SDRAM_BASE;
    uint32_t rw_start = word(header + 12) - S3C2400_SDRAM_BASE;
    uint32_t rw_end = word(header + 16) - S3C2400_SDRAM_BASE;
    uint32_t zi_start = word(header + 20) - S3C2400_SDRAM_BASE;
    uint32_t zi_end = word(header + 24) - S3C2400_SDRAM_BASE;
    if (payload)
        gp32_ram_copy_in(soc->ram, 0, payload, size);
    else
        gp32_ram_copy(soc->ram, 0, source, size);
    /* Keep the packed input intact for startup wrappers. A disjoint RW
       section can be initialized from the file's contiguous RO/RW data. */
    if (rw_start >= size) {
        size_t rw_size = rw_end - rw_start;
        size_t initialized = size - ro_end;
        if (initialized > rw_size)
            initialized = rw_size;
        gp32_ram_copy(soc->ram, rw_start, ro_end, initialized);
        gp32_ram_fill(soc->ram, rw_start + (uint32_t)initialized, 0, rw_size - initialized);
    }
    /* The RW limit usually covers ZI as well, so only the loaded bytes
       bound the section to clear. */
    if (zi_start >= size && zi_start >= rw_start && zi_end >= zi_start && zi_end <= limit)
        gp32_ram_fill(soc->ram, zi_start, 0, zi_end - zi_start);
    *loaded_size = size;
    return NULL;
}

const char *gp32_exec_load_ram(S3c2400 *soc, const uint8_t *data, size_t size, size_t limit, size_t *loaded_size)
{
    const uint8_t *payload;
    uint8_t *allocation;
    const char *error = decode_application(data, size, &payload, &size, &allocation);
    if (error)
        return error;
    error = install(soc, payload, payload, 0, size, limit, loaded_size);
    free(allocation);
    return error;
}

#ifdef M0_GP32_PAGED_RAM
const char *gp32_exec_load_guest(S3c2400 *soc, uint32_t source, size_t size, size_t limit, size_t *loaded_size)
{
    uint8_t header[HEADER];
    if (size < HEADER)
        return "Expected a GP32 GXB application";
    for (unsigned i = 0; i < HEADER; ++i)
        header[i] = *GP32_RAM(soc->ram, source + i);
    if (!valid_gxb(header, size))
        return "Expected a GP32 GXB application";
    return install(soc, header, NULL, source, size, limit, loaded_size);
}
#endif

const char *gp32_exec_boot_image(const uint8_t *bios, const uint8_t *data, size_t size,
                                uint8_t **image, size_t *image_size)
{
    const uint8_t *payload = NULL;
    uint8_t *decoded = NULL;
    if (size == GP32_BIOS_SIZE && (word(data) & 0xff000000u) == 0xea000000u &&
        !valid_gxb(data, size)) {
        bios = data;
    } else {
        const char *error = decode_application(data, size, &payload, &size, &decoded);
        if (error)
            return error;
    }
    size_t padded = (size + 3u) & ~(size_t)3u;
    size_t end = SLOT + 4 + padded;
    size_t length = payload && end > GP32_BIOS_SIZE ? end : GP32_BIOS_SIZE;
    uint8_t *rom = calloc(1, length);
    if (!rom) {
        free(decoded);
        return "Out of memory preparing GP32 boot image";
    }
    memcpy(rom, bios, GP32_BIOS_SIZE);
    if (payload) {
        for (unsigned i = 0; i < 4; ++i)
            rom[SLOT + i] = padded >> (i * 8);
        memcpy(rom + SLOT + 4, payload, size);
    }
    free(decoded);
    *image = rom;
    *image_size = length;
    return NULL;
}
