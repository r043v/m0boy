/* A host directory presented as a SmartMedia card (gp32_vcard.h).

   A physical block the guest never wrote is generated from the layout taken
   when the card was opened.  A written one lives in the swap file, whole.
   Before a host file is changed, the blocks still generated from it move to the
   swap file, so the guest's card never changes under it. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "gp32_vcard.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Files and directories a card can hold; each costs about 80 bytes. */
#ifndef M0_GP32_VCARD_NODES
#define M0_GP32_VCARD_NODES 512
#endif

enum {
    SECTOR = 512, SPARE = 16, PAGE = SECTOR + SPARE, PAGES = 32,
    BLOCK = PAGES * PAGE, WINDOWS = BLOCK / SECTOR, /* 33 exactly */
    BLOCKS = 2048, ZONE_BLOCKS = 1024, ZONE_LOGICAL = 1000, LOGICAL = 2 * ZONE_LOGICAL,
    SECTORS = LOGICAL * PAGES,
    PARTITION = 35, VOLUME = SECTORS - PARTITION - 1,
    CLUSTER_SECTORS = 4, CLUSTER = CLUSTER_SECTORS * SECTOR,
    FATS = 2, FAT_SECTORS = 63, ROOT_ENTRIES = 256, ROOT_SECTORS = ROOT_ENTRIES / 16,
    FAT_AT = PARTITION + 1, ROOT_AT = FAT_AT + FATS * FAT_SECTORS,
    DATA_AT = ROOT_AT + ROOT_SECTORS,
    CLUSTERS = (SECTORS - 1 - DATA_AT) / CLUSTER_SECTORS,
    NODES = M0_GP32_VCARD_NODES, DEPTH = 8, LINES = 16, HEADER = 1024, PATH = 512, ROOT = 0
};

typedef struct {
    char name[11];   /* as in the FAT directory */
    char host[13];   /* as in the host directory */
    uint8_t directory, seen, pending;
    uint16_t parent; /* index in the same table; entry 0 is the root */
    uint16_t first;  /* first cluster, 0 for an empty file */
    uint32_t size;
} Node;

typedef struct {
    Node node[NODES];
    unsigned count;
} Tree;

typedef struct Guest Guest;

static struct Vcard {
    char folder[PATH], swap_path[PATH];
    Gp32VcardReport report;
    FILE *swap;
    int source; /* the file being read: its descriptor + 1, 0 when none */
    unsigned source_node;
    uint32_t source_at; /* where the open source file stands */
    Tree layout, host; /* the card as opened; the directory as it is now */
    Guest *work;       /* reserved at opening: a sync must not run out of memory */
    unsigned header_size, slots;
    uint16_t length[NODES];          /* clusters of each node of the layout */
    uint32_t writes;
    uint8_t header[HEADER];
    uint16_t slot[BLOCKS];           /* swap slot + 1, 0 while generated */
    uint8_t dirty[BLOCKS / 8];       /* written since the last sync */
    uint32_t tag[LINES];             /* raw window + 1 */
    uint8_t modified[LINES];
    uint8_t line[LINES][SECTOR] __attribute__((aligned(4)));
    uint32_t kept_page;              /* the page last generated + 1, 0 when none */
    uint8_t kept[PAGE];
} *card;

static void report(const char *message, const char *path)
{
    if (card && card->report)
        card->report(message, path ? path : "");
}

static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* ---- names and paths ---------------------------------------------------- */

static bool short_name(const char *host, char name[11])
{
    static const char marks[] = "_-!#$%&'()@^`{}~";
    unsigned at = 0, end = 8;
    memset(name, ' ', 11);
    for (const char *p = host; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '.') {
            if (!at || end != 8 || !p[1])
                return false;
            at = 8;
            end = 11;
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(marks, c)) || at >= end)
            return false;
        name[at++] = (char)c;
    }
    return at != 0;
}

/* "NAME    EXT" back to "NAME.EXT"; false for what a host name cannot hold. */
static bool host_name(const uint8_t name[11], char host[13])
{
    unsigned n = 0;
    for (unsigned i = 0; i < 11; ++i) {
        if (i == 8 && name[8] != ' ')
            host[n++] = '.';
        if (name[i] == ' ')
            continue;
        if (name[i] < 0x20 || name[i] >= 0x7f || strchr("/\\:*?\"<>|.", name[i]))
            return false;
        host[n++] = (char)name[i];
    }
    host[n] = 0;
    return n != 0 && name[0] != ' ';
}

static bool node_path(const Tree *tree, unsigned index, char path[PATH])
{
    unsigned chain[DEPTH], depth = 0;
    for (unsigned i = index; i != ROOT; i = tree->node[i].parent) {
        if (depth == DEPTH)
            return false;
        chain[depth++] = i;
    }
    size_t at = (size_t)snprintf(path, PATH, "%s", card->folder);
    while (depth) {
        if (at >= PATH)
            return false;
        at += (size_t)snprintf(path + at, PATH - at, "/%s", tree->node[chain[--depth]].host);
    }
    return at < PATH;
}

/* ---- the layout taken at opening ---------------------------------------- */

static bool scan(Tree *tree, unsigned parent, unsigned depth)
{
    char path[PATH];
    if (!node_path(tree, parent, path))
        return false;
    DIR *dir = opendir(path);
    if (!dir) {
        report("cannot list the directory", path);
        return false;
    }
    unsigned first = tree->count;
    struct dirent *entry;
    bool good = true;
    while (good && (entry = readdir(dir))) {
        const char *host = entry->d_name;
        if (!strcmp(host, ".") || !strcmp(host, "..") || !strcmp(host, "~SYNC.TMP") ||
            (parent == ROOT && !strcasecmp(host, "CARD.UID")))
            continue;
        char name[11], child[PATH];
        if (!short_name(host, name)) {
            report("not an 8.3 name, left off the card", host);
            continue;
        }
        struct stat info;
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, host) >= sizeof child ||
            stat(child, &info) || !(S_ISDIR(info.st_mode) || S_ISREG(info.st_mode))) {
            report("unreadable entry, left off the card", host);
            continue;
        }
        for (unsigned i = first; i < tree->count; ++i)
            if (!memcmp(tree->node[i].name, name, 11)) {
                report("two names differ only by case", child);
                good = false;
            }
        if (tree->count == NODES || (S_ISREG(info.st_mode) && info.st_size > CLUSTERS * (long)CLUSTER)) {
            report("too many files or too large a file for the card", child);
            good = false;
        }
        if (!good)
            break;
        Node *node = &tree->node[tree->count++];
        *node = (Node){.parent = (uint16_t)parent, .directory = S_ISDIR(info.st_mode) != 0,
                       .size = S_ISDIR(info.st_mode) ? 0 : (uint32_t)info.st_size};
        memcpy(node->name, name, 11);
        strcpy(node->host, host); /* an 8.3 name fits */
    }
    closedir(dir);
    unsigned end = tree->count;
    for (unsigned i = first; good && i < end; ++i)
        if (tree->node[i].directory) {
            if (depth + 1 >= DEPTH) {
                report("directories nested too deep", tree->node[i].host);
                return false;
            }
            good = scan(tree, i, depth + 1);
        }
    return good;
}

static unsigned children(const Tree *tree, unsigned parent)
{
    unsigned n = 0;
    for (unsigned i = 1; i < tree->count; ++i)
        n += tree->node[i].parent == parent;
    return n;
}

static unsigned node_clusters(const Tree *tree, unsigned index)
{
    const Node *node = &tree->node[index];
    uint32_t bytes = node->directory ? (children(tree, index) + 2u) * 32u : node->size;
    return (bytes + CLUSTER - 1) / CLUSTER;
}

static bool allocate(Tree *tree)
{
    if (children(tree, ROOT) > ROOT_ENTRIES) {
        report("too many entries in the root directory", card->folder);
        return false;
    }
    unsigned next = 2;
    for (unsigned i = 1; i < tree->count; ++i) {
        unsigned n = node_clusters(tree, i);
        if (n > CLUSTERS + 2 - next) {
            report("the directory does not fit on a 32 MiB card", card->folder);
            return false;
        }
        tree->node[i].first = (uint16_t)(n ? next : 0);
        card->length[i] = (uint16_t)n;
        next += n;
    }
    return true;
}

/* The node owning a cluster of the layout, 0 when it is free.  Nodes hold
   increasing clusters. */
static unsigned cluster_owner(unsigned cluster)
{
    for (unsigned i = 1; i < card->layout.count; ++i) {
        unsigned first = card->layout.node[i].first;
        if (first && cluster >= first && cluster - first < card->length[i])
            return i;
    }
    return 0;
}

/* ---- the generated card -------------------------------------------------- */

static void directory_entry(uint8_t *out, const char name[11], bool directory, unsigned cluster,
                            uint32_t size)
{
    memcpy(out, name, 11);
    out[11] = directory ? 0x10 : 0x20;
    put16(out + 16, 0x21); /* 1980-01-01 */
    put16(out + 18, 0x21);
    put16(out + 24, 0x21);
    put16(out + 26, cluster);
    put32(out + 28, size);
}

/* Sector `index` (16 entries) of a directory of the layout. */
static void directory_sector(unsigned dir, unsigned index, uint8_t *out)
{
    unsigned first = index * 16u, n = 0;
    if (dir != ROOT) {
        unsigned parent = card->layout.node[dir].parent;
        for (unsigned i = 0; i < 2; ++i, ++n)
            if (n >= first && n < first + 16)
                directory_entry(out + (n - first) * 32,
                                i ? "..         " : ".          ", true,
                                i ? (parent ? card->layout.node[parent].first : 0)
                                  : card->layout.node[dir].first, 0);
    }
    for (unsigned i = 1; i < card->layout.count && n < first + 16; ++i) {
        const Node *node = &card->layout.node[i];
        if (node->parent != dir)
            continue;
        if (n >= first)
            directory_entry(out + (n - first) * 32, node->name, node->directory != 0,
                            node->first, node->size);
        ++n;
    }
}

static void close_source(void)
{
    if (card->source)
        close(card->source - 1);
    card->source = 0;
}

static void file_sector(unsigned index, uint32_t offset, uint8_t *out)
{
    if (card->source_node != index) {
        char path[PATH];
        close_source();
        card->source_node = index;
        card->source = node_path(&card->layout, index, path)
                           ? open(path, O_RDONLY | O_BINARY) + 1 : 0;
        if (!card->source)
            report("cannot read the file", card->layout.node[index].host);
        card->source_at = 0;
    }
    /* No stream: the C library of a small host copies and locks a lot.  A seek
       there walks the file's cluster chain: none for the sector which follows
       the last one. */
    if (card->source && (card->source_at == offset ||
                         lseek(card->source - 1, (off_t)offset, SEEK_SET) == (off_t)offset)) {
        int n = (int)read(card->source - 1, out, SECTOR); /* the rest stays zero */
        card->source_at = n == (int)SECTOR ? offset + SECTOR : UINT32_MAX;
    }
}

static void logical_sector(unsigned sector, uint8_t *out)
{
    memset(out, 0, SECTOR);
    if (!sector) {
        static const uint8_t partition[8] = {0, 1, 1, 0, 6, 0xfe, 0xff, 0xff};
        memcpy(out + 446, partition, sizeof partition);
        put32(out + 454, PARTITION);
        put32(out + 458, VOLUME);
        put16(out + 510, 0xaa55);
    } else if (sector == PARTITION) {
        memcpy(out, "\xeb\x3c\x90mkfs.fat", 11);
        put16(out + 11, SECTOR);
        out[13] = CLUSTER_SECTORS;
        put16(out + 14, 1);
        out[16] = FATS;
        put16(out + 17, ROOT_ENTRIES);
        put16(out + 19, VOLUME);
        out[21] = 0xf8;
        put16(out + 22, FAT_SECTORS);
        put16(out + 24, 16);
        put16(out + 26, 8);
        put32(out + 28, PARTITION);
        out[36] = 0x80;
        out[38] = 0x29;
        put32(out + 39, 0x47503332);
        memcpy(out + 43, "NO NAME    FAT16   ", 19);
        put16(out + 510, 0xaa55);
    } else if (sector >= FAT_AT && sector < ROOT_AT) {
        unsigned index = sector - FAT_AT;
        if (index >= FAT_SECTORS)
            index -= FAT_SECTORS;
        for (unsigned i = 0; i < SECTOR / 2; ++i) {
            unsigned cluster = index * (SECTOR / 2) + i, value = 0;
            if (cluster < 2) {
                value = cluster ? 0xffff : 0xfff8;
            } else if (cluster < CLUSTERS + 2) {
                unsigned owner = cluster_owner(cluster);
                if (owner)
                    value = cluster - card->layout.node[owner].first + 1 == card->length[owner]
                                ? 0xffff : cluster + 1;
            }
            put16(out + 2 * i, value);
        }
    } else if (sector >= ROOT_AT && sector < DATA_AT) {
        directory_sector(ROOT, sector - ROOT_AT, out);
    } else if (sector >= DATA_AT) {
        unsigned offset = sector - DATA_AT, cluster = offset / CLUSTER_SECTORS + 2;
        unsigned owner = cluster < CLUSTERS + 2 ? cluster_owner(cluster) : 0;
        if (!owner)
            return;
        unsigned in_node = (cluster - card->layout.node[owner].first) * CLUSTER_SECTORS +
                           offset % CLUSTER_SECTORS;
        if (card->layout.node[owner].directory)
            directory_sector(owner, in_node, out);
        else
            file_sector(owner, in_node * SECTOR, out);
    }
}

/* SmartMedia Hamming code of a 256-byte half page. */
static inline unsigned parity(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    return 0x6996u >> (value & 15u) & 1u;
}

static void ecc(const uint8_t *data, uint8_t out[3])
{
    /* Row parity bit 2b+1 covers the odd bytes with address bit b set, bit 2b
       those with it clear: the former are the XOR of the odd bytes' addresses,
       the latter that XOR the parity of their number.  Four bytes at a time
       (little-endian): a word gives the address bits above its own bytes, the
       XOR of all words gives the two lowest ones and the column parities. */
    uint32_t all = 0;
    unsigned set = 0, row = 0;
    for (unsigned address = 0; address < 256; address += 4) {
        uint32_t word;
        memcpy(&word, data + address, 4);
        all ^= word;
        set ^= address & -parity(word);
    }
    unsigned count = parity(all);
    set |= parity(all & 0xff00ff00u) | parity(all & 0xffff0000u) << 1;
    unsigned column = (all ^ all >> 8 ^ all >> 16 ^ all >> 24) & 0xffu;
    for (unsigned bit = 0; bit < 8; ++bit) {
        unsigned high = set >> bit & 1u;
        row |= (high << 1 | (high ^ count)) << (2 * bit);
    }
    static const uint8_t mask[6] = {0x55, 0xaa, 0x33, 0xcc, 0x0f, 0xf0};
    unsigned code = 3;
    for (unsigned i = 0; i < 6; ++i)
        code |= (1u ^ (unsigned)__builtin_parity(column & mask[i])) << (i + 2);
    put16(out, row ^ 0xffff);
    out[2] = (uint8_t)code;
}

/* The logical block a physical one holds on the untouched card, -1 when it is
   the CIS block or erased. */
static int pristine_logical(unsigned block, unsigned *address)
{
    unsigned zone = block / ZONE_BLOCKS;
    int index = (int)(block % ZONE_BLOCKS) - (zone == 0);
    if (index < 0 || index >= ZONE_LOGICAL)
        return -1;
    unsigned a = 0x1000u | (unsigned)index << 1;
    if (address)
        *address = a | (unsigned)__builtin_parity(a);
    return (int)(zone * ZONE_LOGICAL) + index;
}

static void generate_page(unsigned block, unsigned page, uint8_t out[PAGE])
{
    unsigned address = 0;
    int logical = pristine_logical(block, &address);
    memset(out, 0xff, PAGE);
    if (logical >= 0) {
        logical_sector((unsigned)logical * PAGES + page, out);
    } else if (!block && !page) {
        static const uint8_t cis[16] = {0x01, 0x03, 0xd9, 0x01, 0xff, 0x18, 0x02, 0xdf,
                                        0x01, 0x20, 0x04, 0x00, 0x00, 0x00, 0x00, 0xff};
        memcpy(out, cis, sizeof cis);
        memcpy(out + 256, cis, sizeof cis);
    } else {
        return; /* erased */
    }
    /* Spare: six erased bytes, then twice the block address and the code of a
       half page, the second half first. */
    uint8_t *spare = out + SECTOR;
    spare[6] = spare[11] = (uint8_t)(address >> 8);
    spare[7] = spare[12] = (uint8_t)address;
    ecc(out + 256, spare + 8);
    ecc(out, spare + 13);
}

/* One 512-byte window of a generated block. */
static void generate_window(unsigned block, unsigned window, uint8_t *out)
{
    unsigned at = window * SECTOR, done = 0;
    while (done < SECTOR) {
        unsigned index = (at + done) / PAGE, column = (at + done) % PAGE;
        unsigned n = PAGE - column;
        if (n > SECTOR - done)
            n = SECTOR - done;
        /* Windows overlap pages by turns: keeping the last one reads each
           file sector once, in order.  A generated page never changes: what
           the guest or a sync alters is moved to the swap file first. */
        uint32_t key = block * PAGES + index + 1;
        if (card->kept_page != key) {
            generate_page(block, index, card->kept);
            card->kept_page = key;
        }
        memcpy(out + done, card->kept + column, n);
        done += n;
    }
}

/* ---- the swap file and the cache ----------------------------------------- */

static bool swap_seek(unsigned block, unsigned window)
{
    return !fseek(card->swap, (long)((card->slot[block] - 1u) * (uint32_t)BLOCK +
                                     window * SECTOR), SEEK_SET);
}

/* Give a block its place in the swap file, with its present content. */
static bool materialize(unsigned block)
{
    if (card->slot[block])
        return true;
    uint8_t window[SECTOR];
    card->slot[block] = (uint16_t)(card->slots + 1);
    bool good = swap_seek(block, 0);
    for (unsigned i = 0; good && i < WINDOWS; ++i) {
        generate_window(block, i, window);
        good = fwrite(window, 1, SECTOR, card->swap) == SECTOR;
    }
    if (!good) {
        card->slot[block] = 0;
        report("cannot grow the swap file", card->swap_path);
        return false;
    }
    ++card->slots;
    return true;
}

static void flush_line(unsigned i)
{
    if (!card->modified[i])
        return;
    uint32_t window = card->tag[i] - 1;
    unsigned block = window / WINDOWS, in_block = window % WINDOWS;
    if (!materialize(block) || !swap_seek(block, in_block) ||
        fwrite(card->line[i], 1, SECTOR, card->swap) != SECTOR)
        report("card write lost", card->swap_path);
    card->modified[i] = 0;
}

uint8_t *gp32_vcard_bytes(size_t offset, bool write)
{
    /* The identity header keeps the raw windows aligned: 1024 bytes. */
    if (offset < card->header_size)
        return card->header + offset;
    uint32_t raw = (uint32_t)(offset - card->header_size), window = raw / SECTOR;
    unsigned i = window % LINES;
    if (card->tag[i] != window + 1) {
        flush_line(i);
        unsigned block = window / WINDOWS, in_block = window % WINDOWS;
        if (block >= BLOCKS) {
            memset(card->line[i], 0xff, SECTOR);
        } else if (!card->slot[block]) {
            generate_window(block, in_block, card->line[i]);
        } else if (!swap_seek(block, in_block) ||
                   fread(card->line[i], 1, SECTOR, card->swap) != SECTOR) {
            report("cannot read the swap file", card->swap_path);
            memset(card->line[i], 0xff, SECTOR);
        }
        card->tag[i] = window + 1;
    }
    if (write) {
        unsigned block = window / WINDOWS;
        if (block < BLOCKS) {
            card->modified[i] = 1;
            card->dirty[block / 8] |= (uint8_t)(1u << (block % 8));
            ++card->writes;
        }
    }
    return card->line[i] + raw % SECTOR;
}

void gp32_vcard_flush(void)
{
    if (card)
        for (unsigned i = 0; i < LINES; ++i)
            flush_line(i);
}

uint32_t gp32_vcard_writes(void) { return card ? card->writes : 0; }

/* ---- reading the guest's file system back -------------------------------- */

struct Guest {
    uint16_t map[LOGICAL];     /* physical block + 1 */
    uint8_t used[(CLUSTERS + 2 + 7) / 8];
    unsigned cluster_sectors, fat_at, root_at, root_sectors, data_at, clusters;
    uint32_t fat_cached;       /* sector + 1 */
    uint8_t fat[SECTOR];
    Tree tree;
    uint16_t match[NODES];     /* host entry -> guest entry */
};

static bool raw_read(uint32_t offset, uint8_t *out, unsigned n)
{
    while (n) {
        unsigned run = SECTOR - offset % SECTOR;
        if (run > n)
            run = n;
        memcpy(out, gp32_vcard_bytes(card->header_size + (size_t)offset, false), run);
        out += run;
        offset += run;
        n -= run;
    }
    return true;
}

static bool map_blocks(Guest *g)
{
    uint8_t best[LOGICAL] = {0}, twice[LOGICAL] = {0};
    for (unsigned block = 0; block < BLOCKS; ++block) {
        int logical;
        unsigned pages = PAGES;
        if (!card->slot[block]) {
            logical = pristine_logical(block, NULL);
        } else {
            uint8_t spare[SPARE];
            raw_read(block * BLOCK + SECTOR, spare, SPARE);
            unsigned address = (unsigned)spare[6] << 8 | spare[7];
            if (__builtin_popcount(spare[5]) < 7 || address >> 12 != 1 ||
                __builtin_parity(address))
                continue;
            unsigned index = (address & 0xfff) >> 1;
            if (index >= ZONE_LOGICAL)
                continue;
            logical = (int)(block / ZONE_BLOCKS * ZONE_LOGICAL + index);
            pages = 0;
            for (unsigned page = 0; page < PAGES; ++page) {
                uint8_t tag[2];
                raw_read(block * BLOCK + page * PAGE + SECTOR + 6, tag, 2);
                pages += tag[0] == spare[6] && tag[1] == spare[7];
            }
        }
        if (logical < 0)
            continue;
        /* A rewrite stopped half way leaves a partial copy beside the complete
           one. */
        if (pages > best[logical]) {
            best[logical] = (uint8_t)pages;
            twice[logical] = 0;
            g->map[logical] = (uint16_t)(block + 1);
        } else if (pages == best[logical]) {
            twice[logical] = 1;
        }
    }
    for (unsigned i = 0; i < LOGICAL; ++i)
        if (twice[i]) {
            report("two copies of one logical block", "");
            return false;
        }
    return true;
}

static bool guest_sector(Guest *g, uint32_t sector, uint8_t *out)
{
    if (sector >= SECTORS)
        return false;
    unsigned block = g->map[sector / PAGES];
    if (!block) {
        memset(out, 0, SECTOR);
        return true;
    }
    return raw_read((block - 1u) * BLOCK + sector % PAGES * PAGE, out, SECTOR);
}

static bool guest_geometry(Guest *g)
{
    uint8_t s[SECTOR];
    if (!guest_sector(g, 0, s) || get16(s + 510) != 0xaa55 ||
        (s[450] != 1 && s[450] != 4 && s[450] != 6 && s[450] != 0x0e))
        return false;
    uint32_t start = get32(s + 454);
    if (!start || start >= SECTORS || !guest_sector(g, start, s) || get16(s + 510) != 0xaa55 ||
        get16(s + 11) != SECTOR)
        return false;
    unsigned spc = s[13], reserved = get16(s + 14), fats = s[16], entries = get16(s + 17);
    unsigned fat_sectors = get16(s + 22);
    uint32_t total = get16(s + 19) ? get16(s + 19) : get32(s + 32);
    if (!spc || (spc & (spc - 1)) || !reserved || !fats || fats > 2 || !entries || !fat_sectors ||
        total > SECTORS - start)
        return false;
    g->cluster_sectors = spc;
    g->fat_at = start + reserved;
    g->root_at = g->fat_at + fats * fat_sectors;
    g->root_sectors = (entries + 15) / 16;
    g->data_at = g->root_at + g->root_sectors;
    if (g->data_at - start >= total)
        return false;
    g->clusters = (total - (g->data_at - start)) / spc;
    /* FAT16 only: 12-bit entries below 4085 clusters. */
    return g->clusters >= 4085 && g->clusters <= CLUSTERS * CLUSTER_SECTORS / spc &&
           g->clusters <= CLUSTERS && (g->clusters + 2) * 2 <= fat_sectors * SECTOR;
}

static unsigned fat_entry(Guest *g, unsigned cluster)
{
    uint32_t sector = g->fat_at + cluster / (SECTOR / 2);
    if (g->fat_cached != sector + 1) {
        if (!guest_sector(g, sector, g->fat))
            return 0;
        g->fat_cached = sector + 1;
    }
    return get16(g->fat + cluster % (SECTOR / 2) * 2);
}

/* A sound chain ends and shares no cluster.  The file system updates a file's
   entry when it closes it: until then the chain of a file can be longer or
   shorter than its size says, and the file is pending. */
typedef enum { CHAIN_BROKEN, CHAIN_COMPLETE, CHAIN_PENDING } Chain;

static Chain claim_chain(Guest *g, unsigned cluster, uint32_t size, bool directory)
{
    uint32_t bytes = g->cluster_sectors * SECTOR;
    uint32_t wanted = directory ? 0 : (size + bytes - 1) / bytes, n = 0;
    if (!cluster)
        return directory ? CHAIN_BROKEN : size ? CHAIN_PENDING : CHAIN_COMPLETE;
    while (cluster >= 2 && cluster < g->clusters + 2) {
        if (g->used[cluster / 8] & 1u << cluster % 8)
            return CHAIN_BROKEN;
        g->used[cluster / 8] |= (uint8_t)(1u << cluster % 8);
        ++n;
        cluster = fat_entry(g, cluster);
        if (cluster >= 0xfff8)
            return directory || n == wanted ? CHAIN_COMPLETE : CHAIN_PENDING;
    }
    return CHAIN_BROKEN;
}

static bool read_directory(Guest *g, unsigned parent, unsigned depth)
{
    unsigned cluster = parent ? g->tree.node[parent].first : 0;
    unsigned first = g->tree.count;
    uint8_t s[SECTOR];
    bool end = false;
    for (uint32_t index = 0; !end; ++index) {
        uint32_t sector;
        if (!parent) {
            if (index == g->root_sectors)
                break;
            sector = g->root_at + index;
        } else {
            if (index && !(index % g->cluster_sectors)) {
                cluster = fat_entry(g, cluster);
                if (cluster >= 0xfff8)
                    break;
            }
            sector = g->data_at + (cluster - 2) * g->cluster_sectors +
                     index % g->cluster_sectors;
        }
        if (!guest_sector(g, sector, s))
            return false;
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t *e = s + 32 * i;
            if (!e[0]) {
                end = true;
                break;
            }
            if (e[0] == 0xe5 || e[0] == '.' || (e[11] & 0x08)) /* long names carry 0x0f */
                continue;
            if (g->tree.count == NODES)
                return false;
            Node *node = &g->tree.node[g->tree.count];
            *node = (Node){.parent = (uint16_t)parent, .directory = (e[11] & 0x10) != 0,
                           .first = (uint16_t)get16(e + 26),
                           .size = (e[11] & 0x10) ? 0 : get32(e + 28)};
            memcpy(node->name, e, 11);
            Chain chain = host_name(e, node->host)
                ? claim_chain(g, node->first, node->size, node->directory) : CHAIN_BROKEN;
            if (chain == CHAIN_BROKEN) {
                report("damaged entry on the guest card", node->host);
                return false;
            }
            node->pending = chain == CHAIN_PENDING;
            ++g->tree.count;
        }
    }
    unsigned last = g->tree.count;
    for (unsigned i = first; i < last; ++i)
        if (g->tree.node[i].directory &&
            (depth + 1 >= DEPTH || !read_directory(g, i, depth + 1)))
            return false;
    return true;
}

/* ---- writing the directory ------------------------------------------------ */

/* The entry of `tree` with this name under `parent`, 0 when there is none. */
static unsigned lookup(const Tree *tree, unsigned parent, const char name[11])
{
    for (unsigned i = 1; i < tree->count; ++i)
        if (tree->node[i].parent == parent && !memcmp(tree->node[i].name, name, 11))
            return i;
    return 0;
}

/* Move to the swap file every block still generated from this host file. */
static bool pin_file(const char *path)
{
    for (unsigned i = 1; i < card->layout.count; ++i) {
        const Node *node = &card->layout.node[i];
        char other[PATH];
        if (node->directory || !node->first || !node_path(&card->layout, i, other) ||
            strcasecmp(other, path))
            continue;
        uint32_t sector = DATA_AT + (node->first - 2u) * CLUSTER_SECTORS;
        uint32_t end = sector + (uint32_t)card->length[i] * CLUSTER_SECTORS;
        for (uint32_t logical = sector / PAGES; logical * PAGES < end; ++logical) {
            unsigned zone = logical / ZONE_LOGICAL;
            unsigned block = zone * ZONE_BLOCKS + logical % ZONE_LOGICAL + (zone == 0);
            if (!materialize(block))
                return false;
        }
    }
    close_source();
    card->source_node = 0;
    return true;
}

static bool touched(Guest *g, const Node *node)
{
    unsigned cluster = node->first;
    while (cluster >= 2 && cluster < g->clusters + 2) {
        uint32_t sector = g->data_at + (cluster - 2) * g->cluster_sectors;
        for (unsigned i = 0; i < g->cluster_sectors; ++i) {
            unsigned block = g->map[(sector + i) / PAGES];
            if (block && (card->dirty[(block - 1) / 8] & 1u << (block - 1) % 8))
                return true;
        }
        cluster = fat_entry(g, cluster);
    }
    return false;
}

/* Walk a guest file: write it to `out`, or tell whether `out` already holds
   it. */
static bool file_content(Guest *g, const Node *node, FILE *out, bool compare)
{
    uint8_t s[SECTOR], present[SECTOR];
    unsigned cluster = node->first;
    uint32_t left = node->size;
    while (left) {
        uint32_t sector = g->data_at + (cluster - 2) * g->cluster_sectors;
        for (unsigned i = 0; left && i < g->cluster_sectors; ++i) {
            unsigned n = left < SECTOR ? left : SECTOR;
            if (!guest_sector(g, sector + i, s))
                return false;
            if (compare ? (fread(present, 1, n, out) != n || memcmp(present, s, n))
                        : fwrite(s, 1, n, out) != n)
                return false;
            left -= n;
        }
        if (left)
            cluster = fat_entry(g, cluster);
    }
    return true;
}

static bool export_file(Guest *g, const Node *node, const char *directory, const char *path)
{
    FILE *present = fopen(path, "rb");
    if (present) {
        struct stat info;
        bool same = !stat(path, &info) && (uint32_t)info.st_size == node->size &&
                    file_content(g, node, present, true);
        fclose(present);
        if (same)
            return true;
    }
    char temporary[PATH];
    if ((size_t)snprintf(temporary, sizeof temporary, "%s/~SYNC.TMP", directory) >=
        sizeof temporary)
        return false;
    FILE *out = fopen(temporary, "wb");
    bool good = out && file_content(g, node, out, false);
    if (out && fclose(out))
        good = false;
    if (good && !pin_file(path))
        good = false;
    if (good && rename(temporary, path)) {
        /* FAT refuses to rename over a file. */
        remove(path);
        good = !rename(temporary, path);
    }
    if (!good) {
        report("cannot write the file", path);
        if (out)
            remove(temporary);
    }
    return good;
}

bool gp32_vcard_sync(void)
{
    if (!card)
        return false;
    gp32_vcard_flush();
    Guest *g = card->work;
    memset(g, 0, sizeof *g);
    uint16_t *match = g->match;
    g->tree.count = 1;
    g->tree.node[ROOT].directory = 1;
    bool good = map_blocks(g) && guest_geometry(g) && read_directory(g, ROOT, 0);
    if (!good)
        report("the card holds no complete file system, directory left unchanged", "");
    /* Which host entry each guest entry continues; parents come first. */
    Tree *host = &card->host;
    for (unsigned i = 1; i < (good ? g->tree.count : 0); ++i) {
        Node *node = &g->tree.node[i];
        unsigned parent = 0;
        bool known = !node->parent;
        for (unsigned j = 1; !known && j < host->count; ++j)
            if (match[j] == node->parent) {
                parent = j;
                known = true;
            }
        unsigned j = known ? lookup(host, parent, node->name) : 0;
        if (j && host->node[j].directory == node->directory) {
            match[j] = (uint16_t)i;
            node->seen = 1;
            memcpy(node->host, host->node[j].host, sizeof node->host); /* host spelling */
        }
    }
    /* Remove what the guest removed, children before their directory. */
    for (unsigned j = host->count; good && j-- > 1;) {
        char path[PATH];
        if (match[j] || !node_path(host, j, path))
            continue;
        if (host->node[j].directory) {
            if (rmdir(path))
                report("directory kept, it is not empty", path);
        } else if (!pin_file(path) || (remove(path) && errno != ENOENT)) {
            report("cannot remove the file", path);
            good = false;
        }
    }
    for (unsigned i = 1; good && i < g->tree.count; ++i) {
        Node *node = &g->tree.node[i];
        char path[PATH], directory[PATH];
        if (!node_path(&g->tree, i, path) || !node_path(&g->tree, node->parent, directory)) {
            good = false;
        } else if (node->directory) {
            if (!node->seen && mkdir(path, 0777)) {
                report("cannot create the directory", path);
                good = false;
            }
        } else if (node->pending) {
            report("file still being written, left as it was", path);
            node->size = UINT32_MAX; /* no later size matches: it is then written */
        } else {
            unsigned j = 0;
            for (unsigned k = 1; k < host->count; ++k)
                if (match[k] == i)
                    j = k;
            if (!j || host->node[j].size != node->size || host->node[j].first != node->first ||
                touched(g, node))
                good = export_file(g, node, directory, path);
        }
    }
    if (good) {
        *host = g->tree;
        memset(card->dirty, 0, sizeof card->dirty);
    }
    return good;
}

/* ---- opening ------------------------------------------------------------- */

void gp32_vcard_close(void)
{
    if (!card)
        return;
    close_source();
    if (card->swap) {
        fclose(card->swap);
        remove(card->swap_path);
    }
    free(card->work);
    free(card);
    card = NULL;
}

size_t gp32_vcard_open(const char *folder, const char *swap_path, Gp32VcardReport say)
{
    gp32_vcard_close();
    card = calloc(1, sizeof *card);
    if (!card)
        return 0;
    card->report = say;
    card->work = malloc(sizeof *card->work);
    if (!card->work) {
        report("not enough memory", folder);
        gp32_vcard_close();
        return 0;
    }
    if ((size_t)snprintf(card->folder, PATH, "%s", folder) >= PATH ||
        (size_t)snprintf(card->swap_path, PATH, "%s", swap_path) >= PATH) {
        report("path too long", folder);
        gp32_vcard_close();
        return 0;
    }
    char path[PATH];
    static const char *const required[] = {"GAME", "GPMM"};
    mkdir(card->folder, 0777);
    for (unsigned i = 0; i < 2; ++i) {
        DIR *dir = opendir(card->folder);
        bool found = false;
        struct dirent *entry;
        while (dir && (entry = readdir(dir)))
            found |= !strcasecmp(entry->d_name, required[i]);
        if (dir)
            closedir(dir);
        if (!found && (size_t)snprintf(path, PATH, "%s/%s", card->folder, required[i]) < PATH)
            mkdir(path, 0777);
    }
    card->layout.count = 1;
    card->layout.node[ROOT].directory = 1;
    if (!scan(&card->layout, ROOT, 0) || !allocate(&card->layout)) {
        gp32_vcard_close();
        return 0;
    }
    card->host = card->layout;
    if ((size_t)snprintf(path, PATH, "%s/CARD.UID", card->folder) < PATH) {
        FILE *identity = fopen(path, "rb");
        if (identity) {
            if (fread(card->header, 1, HEADER, identity) == HEADER)
                card->header_size = HEADER;
            else
                report("CARD.UID must hold 1024 bytes", path);
            fclose(identity);
        }
    }
    card->swap = fopen(card->swap_path, "w+b");
    if (!card->swap) {
        report("cannot create the swap file", card->swap_path);
        gp32_vcard_close();
        return 0;
    }
    return card->header_size + (size_t)BLOCKS * BLOCK;
}
