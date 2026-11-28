/* The directory-backed SmartMedia card (gp32_vcard.h), driven through the NAND
   commands the guest uses.  It builds its files in DIRECTORY, which must not
   exist yet, or in a new directory of TMPDIR (kept for inspection). */
#define _POSIX_C_SOURCE 200809L
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gp32_smc.h"
#include "gp32_vcard.h"

uint8_t *gp32_card_bytes(size_t offset, bool write) { return gp32_vcard_bytes(offset, write); }
void gp32_card_flush(void) { gp32_vcard_flush(); }

static unsigned reports;

static void say(const char *message, const char *path)
{
    ++reports;
    fprintf(stderr, "card: %s %s\n", message, path);
}

static char root[600];

static const char *in(const char *name)
{
    static char path[4][800];
    static unsigned at;
    at = (at + 1) % 4;
    snprintf(path[at], sizeof path[at], "%s/%s", root, name);
    return path[at];
}

static void write_file(const char *name, unsigned size, unsigned seed)
{
    FILE *f = fopen(in(name), "wb");
    assert(f);
    for (unsigned i = 0; i < size; ++i)
        fputc((int)((i * 7 + seed + i / 509) & 255), f);
    assert(!fclose(f));
}

static void expect_file(const char *name, unsigned size, unsigned seed)
{
    FILE *f = fopen(in(name), "rb");
    assert(f);
    for (unsigned i = 0; i < size; ++i)
        assert(fgetc(f) == (int)((i * 7 + seed + i / 509) & 255));
    assert(fgetc(f) == EOF);
    fclose(f);
}

static void address(Gp32Smc *card, unsigned row)
{
    gp32_smc_address(card, 0);
    gp32_smc_address(card, (uint8_t)row);
    gp32_smc_address(card, (uint8_t)(row >> 8));
}

static void read_page(Gp32Smc *card, unsigned row, uint8_t out[528])
{
    gp32_smc_command(card, 0x00, true);
    address(card, row);
    for (unsigned i = 0; i < 528; ++i)
        out[i] = gp32_smc_read(card, true);
}

static void program_page(Gp32Smc *card, unsigned row, const uint8_t in_page[528])
{
    gp32_smc_command(card, 0x80, true);
    address(card, row);
    for (unsigned i = 0; i < 528; ++i)
        gp32_smc_write(card, in_page[i]);
    gp32_smc_command(card, 0x10, true);
    assert(!card->failed);
}

static void erase_block(Gp32Smc *card, unsigned row)
{
    gp32_smc_command(card, 0x60, true);
    gp32_smc_address(card, (uint8_t)row);
    gp32_smc_address(card, (uint8_t)(row >> 8));
    gp32_smc_command(card, 0xd0, true);
    assert(!card->failed);
}

/* The guest's view: logical sectors through the block addresses in the spares,
   rewritten the way its driver does it, into an erased block. */
static unsigned map[2000], spare_block[2];

static void mount(Gp32Smc *card)
{
    uint8_t page[528];
    memset(map, 0, sizeof map);
    spare_block[0] = spare_block[1] = 0;
    for (unsigned block = 0; block < 2048; ++block) {
        read_page(card, block * 32, page);
        unsigned a = (unsigned)page[518] << 8 | page[519];
        if (a >> 12 == 1)
            map[(block / 1024) * 1000 + ((a & 0xfff) >> 1)] = block + 1;
        else if (block && a == 0xffff && !spare_block[block / 1024])
            spare_block[block / 1024] = block;
    }
    for (unsigned i = 0; i < 2000; ++i)
        assert(map[i]);
}

static void read_sector(Gp32Smc *card, unsigned sector, uint8_t out[512])
{
    uint8_t page[528];
    read_page(card, (map[sector / 32] - 1) * 32 + sector % 32, page);
    memcpy(out, page, 512);
}

static void write_sector(Gp32Smc *card, unsigned sector, const uint8_t data[512])
{
    unsigned logical = sector / 32, zone = logical / 1000, from = map[logical] - 1;
    unsigned to = spare_block[zone];
    uint8_t page[528], address_bytes[2];
    read_page(card, from * 32, page);
    memcpy(address_bytes, page + 518, 2);
    for (unsigned i = 0; i < 32; ++i) {
        read_page(card, from * 32 + i, page);
        if (i == sector % 32)
            memcpy(page, data, 512);
        memcpy(page + 518, address_bytes, 2);
        memcpy(page + 523, address_bytes, 2);
        program_page(card, to * 32 + i, page);
    }
    erase_block(card, from * 32);
    map[logical] = to + 1;
    spare_block[zone] = from;
}

typedef struct {
    unsigned fat_at, root_at, data_at, cluster;
} Volume;

static Volume volume(Gp32Smc *card)
{
    uint8_t s[512];
    read_sector(card, 0, s);
    unsigned start = s[454] | (unsigned)s[455] << 8;
    read_sector(card, start, s);
    assert(s[510] == 0x55 && s[511] == 0xaa);
    Volume v = {.fat_at = start + s[14], .cluster = s[13]};
    v.root_at = v.fat_at + s[16] * s[22];
    v.data_at = v.root_at + (s[17] | (unsigned)s[18] << 8) / 16;
    return v;
}

/* Find an 11-character name in a directory sector run; returns the sector. */
static unsigned find(Gp32Smc *card, unsigned sector, unsigned sectors, const char *name,
                     uint8_t s[512], unsigned *at)
{
    for (unsigned n = 0; n < sectors; ++n) {
        read_sector(card, sector + n, s);
        for (unsigned i = 0; i < 512; i += 32)
            if (!memcmp(s + i, name, 11)) {
                *at = i;
                return sector + n;
            }
    }
    assert(!"entry not found");
    return 0;
}

static void check_content(Gp32Smc *card, const Volume *v, const uint8_t *entry, unsigned size,
                          unsigned seed)
{
    unsigned cluster = entry[26] | (unsigned)entry[27] << 8;
    assert((entry[28] | (unsigned)entry[29] << 8 | (unsigned)entry[30] << 16) == size);
    uint8_t s[512], fat[512];
    for (unsigned done = 0; done < size;) {
        for (unsigned i = 0; i < v->cluster && done < size; ++i) {
            read_sector(card, v->data_at + (cluster - 2) * v->cluster + i, s);
            for (unsigned k = 0; k < 512 && done < size; ++k, ++done)
                assert(s[k] == (uint8_t)(done * 7 + seed + done / 509));
        }
        read_sector(card, v->fat_at + cluster / 256, fat);
        cluster = fat[cluster % 256 * 2] | (unsigned)fat[cluster % 256 * 2 + 1] << 8;
    }
    assert(cluster == 0xffff);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        snprintf(root, sizeof root, "%s", argv[1]);
        assert(!mkdir(root, 0777));
    } else {
        const char *tmp = getenv("TMPDIR");
        snprintf(root, sizeof root, "%s/vcard.XXXXXX", tmp ? tmp : "/tmp");
        assert(mkdtemp(root));
    }
    assert(!mkdir(in("card"), 0777));
    assert(!mkdir(in("card/GAME"), 0777));
    assert(!mkdir(in("card/GAME/Deep"), 0777));
    write_file("card/GAME/data.bin", 70000, 3);
    write_file("card/GAME/Deep/small.sav", 100, 9);
    write_file("card/GAME/gone.dat", 5000, 11);
    write_file("card/GAME/empty", 0, 0);
    write_file("card/a name too long.txt", 10, 1);

    size_t size = gp32_vcard_open(in("card"), in("swap"), say);
    assert(size == 2048u * 32 * 528);
    assert(reports == 1); /* the long name */
    Gp32Smc card;
    assert(gp32_smc_attach(&card, gp32_vcard_bytes(0, false), size, true));
    mount(&card);
    Volume v = volume(&card);
    uint8_t s[512], game[512], deep[512];
    unsigned at, game_at, deep_at;
    find(&card, v.root_at, 16, "GPMM       ", s, &at);
    find(&card, v.root_at, 16, "GAME       ", s, &at);
    unsigned game_sector = v.data_at + ((s[at + 26] | (unsigned)s[at + 27] << 8) - 2) * v.cluster;
    unsigned data_sector = find(&card, game_sector, v.cluster, "DATA    BIN", game, &game_at);
    check_content(&card, &v, game + game_at, 70000, 3);
    find(&card, game_sector, v.cluster, "DEEP       ", deep, &deep_at);
    unsigned deep_sector =
        v.data_at + ((deep[deep_at + 26] | (unsigned)deep[deep_at + 27] << 8) - 2) * v.cluster;
    find(&card, deep_sector, v.cluster, "SMALL   SAV", deep, &deep_at);
    check_content(&card, &v, deep + deep_at, 100, 9);

    /* Nothing written: a sync leaves everything as it is. */
    assert(!gp32_vcard_writes());
    assert(gp32_vcard_sync());
    expect_file("card/GAME/data.bin", 70000, 3);

    /* The guest rewrites a sector of a file, deletes another and creates one. */
    unsigned first = game[game_at + 26] | (unsigned)game[game_at + 27] << 8;
    uint8_t sector[512];
    read_sector(&card, v.data_at + (first - 2) * v.cluster + 1, sector);
    for (unsigned k = 0; k < 512; ++k)
        sector[k] = 0x5a;
    write_sector(&card, v.data_at + (first - 2) * v.cluster + 1, sector);

    unsigned new_cluster = 9000;
    uint8_t fat[512], content[512];
    read_sector(&card, v.fat_at + new_cluster / 256, fat);
    fat[new_cluster % 256 * 2] = fat[new_cluster % 256 * 2 + 1] = 0xff;
    write_sector(&card, v.fat_at + new_cluster / 256, fat);
    for (unsigned k = 0; k < 512; ++k)
        content[k] = (uint8_t)(k * 7 + 21 + k / 509);
    write_sector(&card, v.data_at + (new_cluster - 2) * v.cluster, content);

    unsigned gone_sector = find(&card, game_sector, v.cluster, "GONE    DAT", game, &game_at);
    game[game_at] = 0xe5;
    unsigned free_at = 0;
    assert(gone_sector == data_sector);
    while (game[free_at])
        free_at += 32;
    memset(game + free_at, 0, 32);
    memcpy(game + free_at, "NEW     SAV", 11);
    game[free_at + 11] = 0x20;
    game[free_at + 26] = (uint8_t)new_cluster;
    game[free_at + 27] = (uint8_t)(new_cluster >> 8);
    game[free_at + 29] = 1; /* 256 bytes */
    write_sector(&card, gone_sector, game);

    assert(gp32_vcard_writes());
    assert(gp32_vcard_sync());
    struct stat info;
    assert(stat(in("card/GAME/gone.dat"), &info));
    assert(stat(in("card/GAME/~SYNC.TMP"), &info));
    expect_file("card/GAME/NEW.SAV", 256, 21);
    expect_file("card/GAME/Deep/small.sav", 100, 9);
    FILE *f = fopen(in("card/GAME/data.bin"), "rb"); /* the host spelling is kept */
    assert(f);
    for (unsigned i = 0; i < 70000; ++i)
        assert(fgetc(f) == (i >= 512 && i < 1024 ? 0x5a : (int)((i * 7 + 3 + i / 509) & 255)));
    assert(fgetc(f) == EOF);
    fclose(f);
    /* The card itself still reads the same once host files have changed. */
    gp32_vcard_flush();
    find(&card, deep_sector, v.cluster, "SMALL   SAV", deep, &deep_at);
    check_content(&card, &v, deep + deep_at, 100, 9);
    read_sector(&card, v.data_at + (first - 2) * v.cluster, sector);
    assert(sector[1] == (uint8_t)(7 + 3));

    /* A file not closed yet (a cluster, still no size) keeps the host copy and
       does not hold the others back; it is written once closed. */
    unsigned open_cluster = 9001;
    read_sector(&card, v.fat_at + open_cluster / 256, fat);
    fat[open_cluster % 256 * 2] = fat[open_cluster % 256 * 2 + 1] = 0xff;
    write_sector(&card, v.fat_at + open_cluster / 256, fat);
    write_sector(&card, v.data_at + (open_cluster - 2) * v.cluster, content);
    read_sector(&card, gone_sector, game);
    unsigned open_at = 0;
    while (game[open_at] && game[open_at] != 0xe5)
        open_at += 32;
    memset(game + open_at, 0, 32);
    memcpy(game + open_at, "OPEN    SAV", 11);
    game[open_at + 11] = 0x20;
    game[open_at + 26] = (uint8_t)open_cluster;
    game[open_at + 27] = (uint8_t)(open_cluster >> 8);
    write_sector(&card, gone_sector, game);
    assert(gp32_vcard_sync());
    assert(stat(in("card/GAME/OPEN.SAV"), &info));
    game[open_at + 29] = 1;
    write_sector(&card, gone_sector, game);
    assert(gp32_vcard_sync());
    expect_file("card/GAME/OPEN.SAV", 256, 21);

    /* A broken file system leaves the directory alone. */
    read_sector(&card, v.fat_at, fat);
    unsigned broken = first;
    fat[broken * 2] = fat[broken * 2 + 1] = 0;
    write_sector(&card, v.fat_at, fat);
    assert(!gp32_vcard_sync());
    expect_file("card/GAME/NEW.SAV", 256, 21);
    assert(!stat(in("card/GAME/data.bin"), &info) && info.st_size == 70000);

    gp32_vcard_close();
    assert(stat(in("swap"), &info));
    puts("ok");
    return 0;
}
