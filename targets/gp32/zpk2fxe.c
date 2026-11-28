/* zpk2fxe: extract the application of a GP32 ZPK package into an FXE file.

   Only the program is converted: the data files a package installs on the
   SmartMedia are not part of an FXE and have to be on the card.

   Build: cc -std=c99 -O2 -o zpk2fxe zpk2fxe.c */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BASE 0x0c000000u
#define RAM_SIZE 0x00800000u
#define MAX_PACKAGE (256u * 1024u * 1024u)

/* ZPK layout: a fixed header, then chunks made of a 20-byte header (tag, size
   at +16) and a payload. GXE describes the executable, GXC holds it. */
enum {
    ZPK_DIRECTORY = 0x6d, ZPK_GXE = 0x175, CHUNK = 20,
    RELATIVE_FIELD = 0x70, BASE_RELATIVE = 0x410, STRIDE_FIELD = 0x560,
    KEY_FIELD = 0x5dc, KEY_SIZE = 96, SCRAMBLED = 256,
    PROFILE_FIELD = 44, PROFILE_PLAIN = 0x228, PROFILE_SPLIT = 0x328,
    GXB_HEADER = 28
};

/* FXE layout: "fxe ", payload size, info size, title, author, 16 reserved
   bytes, icon, payload size, key size, inverted key, XORed payload. */
enum {
    FXE_INFO = 0x454, FXE_TITLE = 12, FXE_AUTHOR = 44, FXE_TEXT = 32,
    FXE_SIZES = 0x45c, FXE_KEY = 0x464, FXE_KEY_SIZE = 4
};

static uint32_t get32(const uint8_t *at)
{
    return at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static void put32(uint8_t *at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        at[i] = (uint8_t)(value >> (8 * i));
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data = NULL;
    long length = -1;
    if (file && !fseek(file, 0, SEEK_END))
        length = ftell(file);
    if (length > 0 && (unsigned long)length <= MAX_PACKAGE && !fseek(file, 0, SEEK_SET))
        data = malloc((size_t)length);
    if (data && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        data = NULL;
    }
    if (file)
        fclose(file);
    *size = data ? (size_t)length : 0;
    return data;
}

/* Returns the decrypted executable (GXB image), or NULL with `error` set. */
static uint8_t *decode_zpk(const uint8_t *data, size_t size, size_t *out_size,
                           const char **error)
{
    const size_t gxe_at = ZPK_GXE + CHUNK;
    *error = "not a ZPK package";
    if (size < gxe_at || memcmp(data, "ZPK ", 4))
        return NULL;
    *error = "unsupported ZPK package layout";
    if (memcmp(data + ZPK_GXE, "GXE ", 4))
        return NULL;
    const uint8_t *gxe = data + gxe_at;
    size_t gxe_size = get32(data + ZPK_GXE + 16);
    if (gxe_size > size - gxe_at || gxe_size < KEY_FIELD + KEY_SIZE)
        return NULL;
    size_t relative = get32(gxe + RELATIVE_FIELD);
    if (relative < BASE_RELATIVE || relative > gxe_size - 0x74 - PROFILE_FIELD - 4)
        return NULL;
    size_t shift = relative - BASE_RELATIVE;
    uint32_t profile = get32(gxe + 0x74 + relative + PROFILE_FIELD);
    if (profile != PROFILE_PLAIN && profile != PROFILE_SPLIT) {
        *error = "unsupported ZPK encryption profile";
        return NULL;
    }
    /* A split package keeps the head of the image in the GXE, before the key. */
    size_t prefix = profile - PROFILE_PLAIN;
    if (shift > gxe_size - KEY_FIELD - KEY_SIZE - prefix)
        return NULL;
    const uint8_t *key = gxe + KEY_FIELD + shift + prefix;
    size_t stride = get32(gxe + STRIDE_FIELD + shift);
    size_t gxc = gxe_at + gxe_size;
    if (size - gxc < 2 * CHUNK)
        return NULL;
    size_t content = gxc + 2 * CHUNK;
    /* At least one shipped package has a blank section tag. */
    if (memcmp(data + gxc, "GXC ", 4) && memcmp(data + gxc, "\0\0\0\0", 4))
        return NULL;
    size_t section = get32(data + gxc + 16);
    size_t length = get32(data + gxc + CHUNK);
    *error = "invalid ZPK executable section";
    if (stride < SCRAMBLED || stride > RAM_SIZE || section < CHUNK ||
        section - CHUNK != length || length > RAM_SIZE - prefix ||
        length > size - content || prefix + length < GXB_HEADER)
        return NULL;
    size_t total = prefix + length;
    uint8_t *out = malloc(total);
    if (!out) {
        *error = "out of memory";
        return NULL;
    }
    memcpy(out, gxe + KEY_FIELD + shift, prefix);
    memcpy(out + prefix, data + content, length);
    /* The first bytes of every stride are XORed with the key. */
    for (size_t offset = 0; offset < total; offset += stride)
        for (size_t i = 0; i < SCRAMBLED && i < total - offset; ++i)
            out[offset + i] ^= key[i % KEY_SIZE];
    /* A wrong key or layout does not produce a GP32 executable header. */
    if ((get32(out) & 0xff000000u) != 0xea000000u || get32(out + 4) != RAM_BASE) {
        *error = "the decrypted data is not a GP32 executable";
        free(out);
        return NULL;
    }
    *out_size = total;
    return out;
}

/* The last bytes of the image serve as the FXE key; loaders append the key to
   the decrypted payload, which restores the whole image. */
static int write_fxe(const char *path, const uint8_t *image, size_t size,
                     const char *title, const char *author)
{
    size_t payload = size - FXE_KEY_SIZE, total = FXE_KEY + size;
    const uint8_t *key = image + payload;
    uint8_t *out = calloc(1, total);
    if (!out)
        return 0;
    memcpy(out, "fxe ", 4);
    put32(out + 4, (uint32_t)payload);
    put32(out + 8, FXE_INFO);
    strncpy((char *)out + FXE_TITLE, title, FXE_TEXT - 1);
    strncpy((char *)out + FXE_AUTHOR, author, FXE_TEXT - 1);
    put32(out + FXE_SIZES, (uint32_t)payload);
    put32(out + FXE_SIZES + 4, FXE_KEY_SIZE);
    for (size_t i = 0; i < FXE_KEY_SIZE; ++i)
        out[FXE_KEY + i] = key[i] ^ 0xff;
    for (size_t i = 0; i < payload; ++i)
        out[FXE_KEY + FXE_KEY_SIZE + i] = image[i] ^ key[i % FXE_KEY_SIZE];
    FILE *file = fopen(path, "wb");
    int ok = file && fwrite(out, 1, total, file) == total;
    if (file && fclose(file))
        ok = 0;
    free(out);
    return ok;
}

/* "dir/Game.zpk" -> "dir/Game.fxe" */
static char *default_output(const char *input)
{
    size_t length = strlen(input), stem = length;
    for (size_t i = length; i > 0 && input[i - 1] != '/' && input[i - 1] != '\\'; --i)
        if (input[i - 1] == '.') {
            stem = i - 1;
            break;
        }
    char *out = malloc(stem + 5);
    if (out) {
        memcpy(out, input, stem);
        strcpy(out + stem, ".fxe");
    }
    return out;
}

static const char *file_name(const char *path)
{
    const char *name = path;
    for (; *path; ++path)
        if (*path == '/' || *path == '\\')
            name = path + 1;
    return name;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *title = NULL, *author = "";
    int usage = 0;
    for (int i = 1; i < argc && !usage; ++i) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc)
            title = argv[++i];
        else if (!strcmp(argv[i], "--author") && i + 1 < argc)
            author = argv[++i];
        else if (argv[i][0] == '-')
            usage = 1;
        else if (!input)
            input = argv[i];
        else if (!output)
            output = argv[i];
        else
            usage = 1;
    }
    if (usage || !input) {
        fprintf(stderr, "usage: %s input.zpk [output.fxe] [--title T] [--author A]\n", argv[0]);
        return 2;
    }
    char *generated = output ? NULL : default_output(input);
    if (!output)
        output = generated;
    if (!output || !strcmp(input, output)) {
        fprintf(stderr, "%s: choose another output file\n", input);
        free(generated);
        return 1;
    }

    size_t size = 0, image_size = 0;
    const char *error = "cannot read the file";
    uint8_t *package = read_file(input, &size);
    uint8_t *image = package ? decode_zpk(package, size, &image_size, &error) : NULL;
    int status = 1;
    if (!image) {
        fprintf(stderr, "%s: %s\n", input, error);
    } else if (!write_fxe(output, image, image_size, title ? title : file_name(output), author)) {
        fprintf(stderr, "%s: cannot write the file\n", output);
    } else {
        printf("%s: %zu bytes of code and data\n", output, image_size);
        const char *directory = (const char *)package + ZPK_DIRECTORY;
        if (!memcmp(directory, "gp:\\", 4) && memchr(directory, 0, ZPK_GXE - ZPK_DIRECTORY))
            printf("The package installs into %s: copy this FXE and the game's data "
                   "files there.\n", directory);
        status = 0;
    }
    free(image);
    free(package);
    free(generated);
    return status;
}
