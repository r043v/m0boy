#define _XOPEN_SOURCE 700
#include "gp32_host.h"
#include "gp32_hle.h"
#include "gp32_exec.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#undef far /* an empty macro there; the CPU has a register of that name */
#define mkdir(path, mode) _mkdir(path)
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

static const uint8_t pduid_mask[] = "SANGHYUK";

static bool parse_id(const char *text, uint8_t id[16])
{
    return strlen(text) == 32 && gp32_exec_parse_id(text, id);
}

static bool read_identity(const char *path, uint8_t id[16], bool *machine)
{
    const char *extension = strrchr(path, '.');
    if (!extension || (strcasecmp(extension, ".gpm") && strcasecmp(extension, ".gps")))
        return false;
    *machine = !strcasecmp(extension, ".gps");
    uint8_t record[64];
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    bool read = fread(record, 1, sizeof(record), file) == sizeof(record) &&
                fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return read && gp32_exec_read_identity(record, id);
}

static uint8_t *read_executable(const char *path, size_t *size)
{
    struct stat info;
    if (stat(path, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > 128L * 1024 * 1024)
        return NULL;
    *size = (size_t)info.st_size;
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    uint8_t *data = malloc(*size);
    bool read = data && fread(data, 1, *size, file) == *size &&
                fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (read)
        return data;
    free(data);
    return NULL;
}

bool gp32_host_identify(const char *path)
{
    uint8_t id[16];
    bool machine;
    if (!read_identity(path, id, &machine)) {
        size_t size;
        uint8_t *data = read_executable(path, &size);
        Gp32ExecIdentity identity = data ? gp32_exec_identity(data, size) : GP32_EXEC_ID_UNKNOWN;
        free(data);
        if (identity == GP32_EXEC_ID_NONE) {
            puts("identity=none");
            return true;
        }
        if (identity == GP32_EXEC_ID_UNKNOWN) {
            fprintf(stderr, "%s: unable to identify a GP32 executable or identity record\n", path);
            return false;
        }
        machine = identity == GP32_EXEC_ID_MACHINE;
        const char *dot = strrchr(path, '.');
        size_t stem = dot ? (size_t)(dot - path) : strlen(path);
        char *sidecar = malloc(stem + 5);
        if (!sidecar)
            return false;
        memcpy(sidecar, path, stem);
        memcpy(sidecar + stem, machine ? ".gps" : ".gpm", 5);
        bool found = read_identity(sidecar, id, &machine);
        if (!found) {
            memcpy(sidecar + stem, machine ? ".GPS" : ".GPM", 5);
            found = read_identity(sidecar, id, &machine);
        }
        free(sidecar);
        if (!found) {
            printf("%s=unavailable\n", machine ? "PDUID" : "SMCID");
            return true;
        }
    }
    printf("%s=", machine ? "PDUID" : "SMCID");
    for (unsigned i = 0; i < sizeof(id); ++i)
        printf("%02x", id[i]);
    putchar('\n');
    return true;
}

bool gp32_host_options(Gp32Host *host, int argc, char **argv)
{
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--readonly")) {
            host->readonly = true;
        } else if (!strcmp(argv[i], "--smc") && i + 1 < argc && !host->card_path) {
            host->card_path = argv[++i];
        } else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc && !host->eeprom_path) {
            host->eeprom_path = argv[++i];
        } else if (!strcmp(argv[i], "--exec") && i + 1 < argc && !host->exec_path) {
            host->exec_path = argv[++i];
        } else if (!strcmp(argv[i], "--smcid") && i + 1 < argc && !host->has_smcid) {
            if (!parse_id(argv[++i], host->smcid))
                return false;
            host->has_smcid = true;
        } else if (!strcmp(argv[i], "--pduid") && i + 1 < argc && !host->has_pduid) {
            if (!parse_id(argv[++i], host->pduid))
                return false;
            host->has_pduid = true;
        } else if (!strcmp(argv[i], "--id-file") && i + 1 < argc) {
            uint8_t id[16];
            bool machine;
            if (!read_identity(argv[++i], id, &machine) ||
                (machine ? host->has_pduid : host->has_smcid)) {
                fprintf(stderr, "Invalid or duplicate GP32 identity record: %s\n", argv[i]);
                return false;
            }
            memcpy(machine ? host->pduid : host->smcid, id, sizeof(id));
            if (machine)
                host->has_pduid = true;
            else
                host->has_smcid = true;
        } else if (!strcmp(argv[i], "--ids")) {
            host->show_ids = true;
        } else {
            return false;
        }
    }
    return true;
}

/* Reuse an existing entry that only differs by case: the card is FAT. */
static bool resolve_entry(char *path, size_t capacity, const char *name, size_t length)
{
    DIR *directory = opendir(path);
    size_t used = strlen(path);
    if (used + length + 2 > capacity) {
        if (directory)
            closedir(directory);
        return false;
    }
    const char *chosen = name;
    struct dirent *entry;
    while (directory && (entry = readdir(directory)))
        if (strlen(entry->d_name) == length && !strncasecmp(entry->d_name, name, length)) {
            chosen = entry->d_name;
            break;
        }
    path[used] = '/';
    memcpy(path + used + 1, chosen, length);
    path[used + 1 + length] = 0;
    if (directory)
        closedir(directory);
    return true;
}

static bool store_resource(void *context, const char *relative, const uint8_t *data, size_t size)
{
    char path[1536];
    snprintf(path, sizeof(path), "%s", (const char *)context);
    while (*relative) {
        size_t length = strcspn(relative, "/");
        bool last = !relative[length];
        if (!resolve_entry(path, sizeof(path), relative, length))
            return false;
        if ((!last || !data) && mkdir(path, 0777) && errno != EEXIST)
            return false;
        relative += length + !last;
    }
    if (!data)
        return true;
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    bool written = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && written;
}

static bool make_directories(char *path)
{
    for (char *slash = strchr(path + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        bool made = !mkdir(path, 0777) || errno == EEXIST;
        *slash = '/';
        if (!made)
            return false;
    }
    return true;
}

static bool has_entry(void *context, const char *path, const uint8_t *data, size_t size)
{
    (void)path; (void)data; (void)size;
    *(bool *)context = true;
    return true;
}

/* A ZPK carries the files its game reads from the card. Unless a card was given,
   unpack them once into a per-game directory, which then keeps the saves. */
/* System software leaves these on every card, and applications save files
   below them without creating them. */
static bool system_directories(const char *card)
{
    static const char *const names[] = {"GPMM", "GPSYS"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        DIR *directory = opendir(card);
        if (!directory)
            return false;
        bool present = false;
        for (struct dirent *entry; !present && (entry = readdir(directory));)
            present = !strcasecmp(entry->d_name, names[i]);
        closedir(directory);
        char path[1040];
        int length = snprintf(path, sizeof(path), "%s/%s", card, names[i]);
        if (!present && (length < 0 || (size_t)length >= sizeof(path) || mkdir(path, 0777)))
            return false;
    }
    return true;
}

static bool package_card(Gp32Host *host, const uint8_t *data, size_t size)
{
    bool found = false;
    const char *error = gp32_exec_resources(data, size, has_entry, &found);
    if (error) {
        fprintf(stderr, "%s: %s\n", host->exec_path, error);
        return false;
    }
    if (!found)
        return true;
#ifdef _WIN32
    /* Directory cards need mkfs.fat and mtools. */
    fprintf(stderr, "%s: this package needs card files; on Windows pass a card image with --smc\n",
            host->exec_path);
    return false;
#else
    const char *root = getenv("M0_GP32_CARDS"), *share = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    const char *name = strrchr(host->exec_path, '/');
    name = name ? name + 1 : host->exec_path;
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    int length;
    if (root && *root)
        length = snprintf(host->package_card, sizeof(host->package_card), "%s/%.*s", root, stem, name);
    else if (share && *share == '/')
        length = snprintf(host->package_card, sizeof(host->package_card), "%s/m0boy-gp32/%.*s", share, stem, name);
    else if (home && *home)
        length = snprintf(host->package_card, sizeof(host->package_card),
                          "%s/.local/share/m0boy-gp32/%.*s", home, stem, name);
    else
        return false;
    if (length < 0 || (size_t)length >= sizeof(host->package_card))
        return false;
    struct stat info;
    if (stat(host->package_card, &info)) {
        char partial[sizeof(host->package_card) + 8];
        snprintf(partial, sizeof(partial), "%s.XXXXXX", host->package_card);
        if (!make_directories(partial) || !mkdtemp(partial)) {
            fprintf(stderr, "%s: %s\n", host->package_card, strerror(errno));
            return false;
        }
        error = gp32_exec_resources(data, size, store_resource, partial);
        if (error || rename(partial, host->package_card)) {
            fprintf(stderr, "%s: %s; partial extraction left in %s\n", host->exec_path,
                    error ? error : strerror(errno), partial);
            return false;
        }
    }
    if (!system_directories(host->package_card)) {
        fprintf(stderr, "%s: %s\n", host->package_card, strerror(errno));
        return false;
    }
    fprintf(stderr, "SmartMedia: %s\n", host->package_card);
    host->card_path = host->package_card;
    return true;
#endif
}

bool gp32_host_boot(Gp32Host *host, S3c2400 *soc, uint8_t **bios,
                    Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks, S3c2400Video video)
{
    if (host->hle) {
        if (!host->exec_path) {
            fprintf(stderr, "--hle requires --exec with a GP32 application\n");
            return false;
        }
        size_t size;
        uint8_t *data = read_executable(host->exec_path, &size);
        const char *error = data ? gp32_hle_boot(soc, data, size, cpu_hooks, bus_hooks, video)
                                 : "Unable to read GP32 executable";
        bool unpacked = error || host->card_path || package_card(host, data, size);
        free(data);
        if (!unpacked)
            return false;
        if (error) {
            fprintf(stderr, "%s: %s\n", host->exec_path, error);
            return false;
        }
        return gp32_host_mount(host, soc);
    }
    size_t rom_size = GP32_BIOS_SIZE;
    if (host->exec_path) {
        const char *path = host->exec_path;
        size_t size;
        uint8_t *data = read_executable(path, &size);
        uint8_t *rom = NULL;
        const char *error = data ? gp32_exec_boot_image(*bios, data, size, &rom, &rom_size)
                                   : "Unable to read GP32 executable";
        bool unpacked = error || host->card_path || package_card(host, data, size);
        free(data);
        if (!unpacked) {
            free(rom);
            return false;
        }
        if (error) {
            fprintf(stderr, "%s: %s\n", path, error);
            return false;
        }
        free(*bios);
        *bios = rom;
    }
    if (!s3c2400_boot(soc, *bios, GP32_BIOS_SIZE, cpu_hooks, bus_hooks, video))
        return false;
    soc->bus.boot_rom_size = rom_size;
    return gp32_host_mount(host, soc);
}

bool gp32_host_step(const Gp32Host *host, S3c2400 *soc, uint64_t *exceptions)
{
    if (host->hle && soc->hle_exited)
        return false;
    uint32_t pc = soc->cpu.r[15];
    if (s3c2400_step(soc) != ARM920T_EXCEPTION)
        return !host->hle || !soc->hle_exited;
    if (exceptions)
        ++*exceptions;
    if (!host->hle)
        return true;
    if (soc->hle_unhandled)
        fprintf(stderr, "Unhandled GP32 HLE SWI %06x at %08x\n", soc->hle_swi, soc->hle_pc);
    else
        fprintf(stderr, "GP32 HLE stopped at exception vector %08x (PC %08x)\n",
                soc->cpu.r[15], pc);
    return false;
}

bool gp32_host_run(const Gp32Host *host, S3c2400 *soc, uint64_t steps, uint64_t *exceptions)
{
    if (host->hle && soc->hle_exited)
        return false;
    uint64_t start = soc->steps, frame = soc->frames;
    while (soc->steps - start < steps && soc->frames == frame) {
        S3c2400Stop stop = s3c2400_run(soc, steps - (soc->steps - start));
        if (stop == S3C2400_STOP_EXIT)
            return false;
        if (stop != S3C2400_STOP_EXCEPTION)
            continue;
        if (exceptions)
            ++*exceptions;
        if (host->hle) {
            if (soc->hle_unhandled)
                fprintf(stderr, "Unhandled GP32 HLE SWI %06x at %08x\n", soc->hle_swi, soc->hle_pc);
            else
                /* r14 of the exception mode names the faulting instruction. */
                fprintf(stderr, "GP32 HLE stopped at exception vector %08x (LR %08x, fault address "
                        "%08x, status %08x, frame %llu)\n", soc->cpu.r[15], soc->cpu.r[14], soc->cpu.far,
                        soc->cpu.dfsr, (unsigned long long)soc->frames);
            return false;
        }
    }
    return true;
}

static void print_id(const char *name, const uint8_t *id)
{
    fprintf(stderr, "%s=", name);
    if (id)
        for (unsigned i = 0; i < 16; ++i)
            fprintf(stderr, "%02x", id[i]);
    else
        fputs("unavailable", stderr);
    fputc('\n', stderr);
}

void gp32_host_print_ids(const Gp32Host *host, const S3c2400 *soc)
{
    if (!host->show_ids)
        return;
    uint8_t id[16];
    bool programmed = false;
    for (unsigned i = 0; i < 16; ++i) {
        programmed |= soc->eeprom[0x10 + i] != 0xff;
        id[i] = soc->eeprom[0x10 + i] ^ pduid_mask[i % 8];
    }
    print_id("PDUID", programmed || host->has_pduid ? id : NULL);
    print_id("SMCID", gp32_smc_uid(&soc->smc));
}

static bool directory_card(Gp32Host *host, char *operation)
{
#ifdef _WIN32
    (void)host; (void)operation;
    fprintf(stderr, "Directory cards are not supported on Windows: use a card image\n");
    return false;
#else
    char *args[] = {"python3", GP32_SYNC_CARD, operation, host->directory, host->snapshot, NULL};
    pid_t child;
    int error = posix_spawnp(&child, args[0], NULL, NULL, args, environ);
    if (error) {
        fprintf(stderr, "SmartMedia directory: %s\n", strerror(error));
        return false;
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static bool open_card(Gp32Host *host, S3c2400 *soc)
{
    struct stat info;
    if (stat(host->card_path, &info))
        return false;
    const char *path = host->card_path;
    if (S_ISDIR(info.st_mode)) {
#ifdef _WIN32
        return directory_card(host, "build");
#else
        host->directory = realpath(path, NULL);
        char pattern[] = "/tmp/m0-gp32-card.XXXXXX";
        if (!host->directory || !mkdtemp(pattern))
            return false;
        snprintf(host->workspace, sizeof(host->workspace), "%s", pattern);
        snprintf(host->snapshot, sizeof(host->snapshot), "%s/card.smc", host->workspace);
        if (!directory_card(host, "build") || stat(host->snapshot, &info))
            return false;
        path = host->snapshot;
#endif
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size > 262144L * 528 + 1024)
        return false;
    host->image_size = (size_t)info.st_size;
    host->image = malloc(host->image_size);
    FILE *file = fopen(path, "rb");
    bool loaded = file && host->image &&
                  fread(host->image, 1, host->image_size, file) == host->image_size;
    if (file)
        fclose(file);
    return loaded && gp32_smc_attach(&soc->smc, host->image, host->image_size, !host->readonly);
}

bool gp32_host_mount(Gp32Host *host, S3c2400 *soc)
{
    if (host->eeprom_path) {
        FILE *file = fopen(host->eeprom_path, "rb");
        bool loaded = false;
        if (file) {
            loaded = fread(soc->eeprom, 1, sizeof(soc->eeprom), file) == sizeof(soc->eeprom) &&
                     fgetc(file) == EOF && !ferror(file);
            fclose(file);
        } else {
            loaded = errno == ENOENT;
        }
        if (!loaded) {
            fprintf(stderr, "Expected a raw %u-byte EEPROM: %s\n", GP32_EEPROM_SIZE, host->eeprom_path);
            return false;
        }
    }
    if (host->has_pduid)
        for (unsigned i = 0; i < 16; ++i)
            soc->eeprom[0x10 + i] = host->pduid[i] ^ pduid_mask[i % 8];
    if (host->card_path && !open_card(host, soc)) {
        fprintf(stderr, "Unable to mount SmartMedia: %s\n", host->card_path);
        return false;
    }
    if (host->has_smcid && !gp32_smc_set_uid(&soc->smc, host->smcid)) {
        fprintf(stderr, "--smcid requires a mounted 512-byte-page SmartMedia card\n");
        return false;
    }
    gp32_host_print_ids(host, soc);
    return true;
}

#ifdef _WIN32
bool gp32_host_save_file(const char *path, const uint8_t *data, size_t size)
{
    size_t length = strlen(path) + sizeof(".tmp");
    char *temp = malloc(length);
    if (!temp)
        return false;
    snprintf(temp, length, "%s.tmp", path);
    FILE *file = fopen(temp, "wbx");
    bool ok = false;
    if (file) {
        bool written = fwrite(data, 1, size, file) == size && fflush(file) == 0 &&
                       _commit(_fileno(file)) == 0;
        ok = fclose(file) == 0 && written &&
             MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING);
        if (!ok)
            remove(temp);
    }
    free(temp);
    return ok;
}
#else
bool gp32_host_save_file(const char *path, const uint8_t *data, size_t size)
{
    char *resolved = realpath(path, NULL);
    if (resolved)
        path = resolved;
    size_t length = strlen(path) + sizeof(".XXXXXX");
    char *temp = malloc(length);
    if (!temp) {
        free(resolved);
        return false;
    }
    snprintf(temp, length, "%s.XXXXXX", path);
    int fd = mkstemp(temp);
    bool ok = false;
    if (fd >= 0) {
        struct stat previous;
        if (!stat(path, &previous))
            fchmod(fd, previous.st_mode & 0777);
        FILE *file = fdopen(fd, "wb");
        if (file) {
            bool written = fwrite(data, 1, size, file) == size && fflush(file) == 0 && fsync(fd) == 0;
            ok = fclose(file) == 0 && written && rename(temp, path) == 0;
        } else {
            close(fd);
        }
        if (!ok)
            unlink(temp);
    }
    free(temp);
    free(resolved);
    return ok;
}
#endif

bool gp32_host_close(Gp32Host *host, const S3c2400 *soc)
{
    bool keep = false, ok = true;
    if (soc && soc->smc.dirty) {
        const char *path = host->directory ? host->snapshot : host->card_path;
        if (!gp32_host_save_file(path, host->image, host->image_size)) {
            fprintf(stderr, "Unable to save SmartMedia: %s\n", path);
            ok = false;
        } else if (host->directory && !directory_card(host, "update")) {
            fprintf(stderr, "Unable to synchronize directory. Updated image preserved at %s\n", path);
            keep = true;
            ok = false;
        }
    }
    if (soc && soc->eeprom_dirty && host->eeprom_path &&
        !gp32_host_save_file(host->eeprom_path, soc->eeprom, sizeof(soc->eeprom))) {
        fprintf(stderr, "Unable to save EEPROM: %s\n", host->eeprom_path);
        ok = false;
    }
#ifndef _WIN32
    if (!keep && host->workspace[0]) {
        char manifest[288];
        snprintf(manifest, sizeof(manifest), "%s/card.json", host->workspace);
        unlink(manifest);
        unlink(host->snapshot);
        rmdir(host->workspace);
    }
#else
    (void)keep;
#endif
    free(host->image);
    free(host->directory);
    return ok;
}
