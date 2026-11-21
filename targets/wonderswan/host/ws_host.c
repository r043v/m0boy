#define _POSIX_C_SOURCE 200809L
#include "ws.h"
#include "m0_host_util.h"
#include "m0_rom_ext.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint8_t *bytes;
    uint32_t *sector_reads;
} HostRom;

static void store_flash(void *context, uint32_t offset, uint32_t count, uint8_t value)
{
    HostRom *rom = context;
    memset(rom->bytes + offset, value, count);
}

static uint8_t read_rom(void *context, uint32_t offset)
{
    HostRom *rom = context;
    if (rom->sector_reads && rom->sector_reads[offset >> 9] != UINT32_MAX)
        ++rom->sector_reads[offset >> 9];
    return rom->bytes[offset];
}

typedef struct {
    unsigned frame;
    bool color, all_frames;
    union {
        uint8_t gray[WS_HEIGHT][WS_WIDTH];
        uint16_t rgb[WS_HEIGHT][WS_WIDTH];
    } pixels;
} Capture;

static void capture_line(void *context, const Ws *ws, unsigned line)
{
    Capture *capture = context;
    if (!capture->all_frames && ws->frames + 1 != capture->frame)
        return;
    if (capture->color)
        ws_render_line_rgb(ws, line, capture->pixels.rgb[line]);
    else
        ws_render_line(ws, line, capture->pixels.gray[line]);
}

static void expand_rgb(const uint16_t row[WS_WIDTH], uint8_t rgb[WS_WIDTH * 3])
{
    for (unsigned x = 0; x < WS_WIDTH; ++x) {
        rgb[3 * x] = (row[x] >> 8) * 17;
        rgb[3 * x + 1] = ((row[x] >> 4) & 15) * 17;
        rgb[3 * x + 2] = (row[x] & 15) * 17;
    }
}

static uint32_t video_crc32(const Capture *capture)
{
    uint8_t rgb[WS_HEIGHT][WS_WIDTH * 3];
    for (unsigned y = 0; y < WS_HEIGHT; ++y) {
        if (capture->color) {
            expand_rgb(capture->pixels.rgb[y], rgb[y]);
        } else {
            for (unsigned x = 0; x < WS_WIDTH; ++x)
                memset(rgb[y] + 3 * x, capture->pixels.gray[y][x], 3);
        }
    }
    return m0_romx_crc32(&rgb[0][0], sizeof(rgb));
}

static void trace_instruction(void *context, uint32_t pc, uint32_t rom_offset)
{
    FILE *trace = context;
    if (rom_offset != UINT32_MAX)
        fprintf(trace, "%08x,%08x\n", pc, rom_offset);
}

static bool save_image(const Ws *ws, const char *path, const Capture *capture, bool color)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;

    bool ok = fprintf(file, "P%d\n%d %d\n255\n", color ? 6 : 5, WS_WIDTH, WS_HEIGHT) > 0;
    for (unsigned y = 0; ok && y < WS_HEIGHT; ++y) {
        if (color) {
            uint16_t pixels[WS_WIDTH];
            if (!capture)
                ws_render_line_rgb(ws, y, pixels);
            const uint16_t *row = capture ? capture->pixels.rgb[y] : pixels;
            uint8_t rgb[WS_WIDTH * 3];
            expand_rgb(row, rgb);
            ok = fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb);
        } else {
            uint8_t pixels[WS_WIDTH];
            if (!capture)
                ws_render_line(ws, y, pixels);
            const uint8_t *row = capture ? capture->pixels.gray[y] : pixels;
            ok = fwrite(row, 1, sizeof(pixels), file) == sizeof(pixels);
        }
    }
    return fclose(file) == 0 && ok;
}

typedef struct {
    FILE *file;
    uint32_t samples;
    bool stereo;
} Wav;

static void wav_le16(FILE *file, uint16_t value)
{
    fputc(value & 255, file);
    fputc(value >> 8, file);
}

static void record_sample(void *context, uint8_t speaker, int16_t left, int16_t right)
{
    Wav *wav = context;
    if (wav->stereo) {
        wav_le16(wav->file, (uint16_t)left);
        wav_le16(wav->file, (uint16_t)right);
    } else {
        fputc(speaker, wav->file);
    }
    ++wav->samples;
}

static bool finish_wav(Wav *wav)
{
    // Reserve the standard PCM header at open and fill it once the length is known.
    unsigned channels = wav->stereo ? 2 : 1;
    unsigned sample_bytes = wav->stereo ? 2 : 1;
    unsigned frame_bytes = channels * sample_bytes;
    uint32_t bytes = wav->samples * frame_bytes;
    uint32_t padding = bytes & 1;
    if (padding)
        fputc(0, wav->file);
    bool ok = fseek(wav->file, 0, SEEK_SET) == 0;
    if (ok) {
        fwrite("RIFF", 1, 4, wav->file);
        m0_le32(wav->file, 36 + bytes + padding);
        fwrite("WAVEfmt ", 1, 8, wav->file);
        m0_le32(wav->file, 16);
        wav_le16(wav->file, 1); // PCM
        wav_le16(wav->file, channels);
        m0_le32(wav->file, WS_AUDIO_RATE);
        m0_le32(wav->file, WS_AUDIO_RATE * frame_bytes);
        wav_le16(wav->file, frame_bytes);
        wav_le16(wav->file, 8 * sample_bytes);
        fwrite("data", 1, 4, wav->file);
        m0_le32(wav->file, bytes);
    }
    ok &= !ferror(wav->file);
    return fclose(wav->file) == 0 && ok;
}

static bool nvram_transfer(FILE *file, Ws *ws, bool write)
{
    uint8_t *regions[] = {ws->internal_nv, ws->cart_nv, ws->sram};
    size_t sizes[] = {ws->internal_eeprom.size, ws->cart_eeprom.size, ws->sram_size};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t size = sizes[i];
        if (!size)
            continue;
        size_t count = write ? fwrite(regions[i], 1, size, file)
                             : fread(regions[i], 1, size, file);
        if (count != size)
            return false;
    }
    if (ws->rtc.present) {
        uint8_t bytes[WS_RTC_SAVE_SIZE];
        if (write) {
            ws_rtc_export(&ws->rtc, bytes);
            return fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
        }
        size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (count == 0)
            return feof(file) && !ferror(file); // Legacy save without a clock.
        if (count != sizeof(bytes) || !ws_rtc_import(&ws->rtc, bytes))
            return false;
    }
    return write || (fgetc(file) == EOF && !ferror(file));
}

static bool load_nvram(const char *path, Ws *ws)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return errno == ENOENT;
    bool ok = nvram_transfer(file, ws, false);
    return fclose(file) == 0 && ok;
}

static bool save_nvram(const char *path, Ws *ws)
{
    size_t size = strlen(path) + sizeof(".tmp.XXXXXX");
    char *temporary = malloc(size);
    if (!temporary)
        return false;
    snprintf(temporary, size, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    bool ok = false;
    if (fd >= 0) {
        FILE *file = fdopen(fd, "wb");
        if (file) {
            ok = nvram_transfer(file, ws, true) && fflush(file) == 0 && fsync(fd) == 0;
            ok = fclose(file) == 0 && ok;
        } else {
            close(fd);
        }
        if (ok)
            ok = rename(temporary, path) == 0;
        if (!ok)
            unlink(temporary);
    }
    free(temporary);
    return ok;
}

int main(int argc, char **argv)
{
    const char *wav_path = NULL, *save_path = NULL, *trace_path = NULL, *bios_path = NULL;
    const char *input_path = NULL, *frame_trace_path = NULL, *exec_trace_path = NULL;
    bool color = false, stereo = false, bizhawk_timing = false;
    while (argc >= 3) {
        if (strcmp(argv[argc - 1], "--color") == 0 && !color) {
            color = true;
            --argc;
            continue;
        }
        if (strcmp(argv[argc - 1], "--bizhawk-timing") == 0 && !bizhawk_timing) {
            bizhawk_timing = true;
            --argc;
            continue;
        }
        if (argc < 4)
            break;
        const char *option = argv[argc - 2];
        if (strcmp(option, "--wav") == 0 && !wav_path)
            wav_path = argv[argc - 1];
        else if (strcmp(option, "--stereo-wav") == 0 && !wav_path) {
            wav_path = argv[argc - 1];
            stereo = true;
        } else if (strcmp(option, "--save") == 0 && !save_path)
            save_path = argv[argc - 1];
        else if (strcmp(option, "--rom-trace") == 0 && !trace_path)
            trace_path = argv[argc - 1];
        else if (strcmp(option, "--bios") == 0 && !bios_path)
            bios_path = argv[argc - 1];
        else if (strcmp(option, "--frame-trace") == 0 && !frame_trace_path)
            frame_trace_path = argv[argc - 1];
        else if (strcmp(option, "--exec-trace") == 0 && !exec_trace_path)
            exec_trace_path = argv[argc - 1];
        else if (strcmp(option, "--input") == 0 && !input_path)
            input_path = argv[argc - 1];
        else
            break;
        argc -= 2;
    }
    if (argc < 2 || argc > 5 || (bios_path && bizhawk_timing)) {
        fprintf(stderr, "Usage: ws_host cartridge.ws [capture.pgm [frames [start-frame]]] [--color] [--bios boot.bin] [--input keys.txt] [--bizhawk-timing] [--wav audio.wav | --stereo-wav audio.wav] [--save nvram.bin] [--rom-trace reads.txt] [--frame-trace frames.csv] [--exec-trace instructions.csv]\n");
        return 2;
    }
    unsigned frame_limit = 0, start_frame = 0;
    for (int arg = 3; arg < argc; ++arg) {
        char *end;
        unsigned long value = strtoul(argv[arg], &end, 10);
        if (end == argv[arg] || *end || !value || value > 100000) {
            fprintf(stderr, "Invalid frame number: %s\n", argv[arg]);
            return 2;
        }
        if (arg == 3)
            frame_limit = value;
        else
            start_frame = value;
    }
    if (frame_trace_path && !frame_limit) {
        fprintf(stderr, "--frame-trace requires a frame count\n");
        return 2;
    }
    if (input_path && (!frame_limit || start_frame)) {
        fprintf(stderr, "--input requires a frame count and replaces start-frame\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        perror(argv[1]);
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 2;
    }
    long size = ftell(file);
    if (size <= 0 || size > WS_MAX_ROM_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Invalid cartridge size\n");
        fclose(file);
        return 2;
    }
    uint8_t *rom = malloc((size_t)size);
    bool loaded = rom && fread(rom, 1, (size_t)size, file) == (size_t)size;
    fclose(file);

    uint8_t bios[WS_COLOR_BIOS_SIZE];
    if (bios_path) {
        file = fopen(bios_path, "rb");
        if (!file) {
            perror(bios_path);
            free(rom);
            return 2;
        }
        unsigned expected = color ? WS_COLOR_BIOS_SIZE : WS_MONO_BIOS_SIZE;
        bool exact = fread(bios, 1, expected, file) == expected &&
                     fgetc(file) == EOF && !ferror(file);
        fclose(file);
        if (!exact) {
            fprintf(stderr, "Expected a %u-byte %s boot ROM: %s\n", expected,
                    color ? "WonderSwan Color" : "WonderSwan", bios_path);
            free(rom);
            return 2;
        }
    }

    uint32_t sector_reads[WS_MAX_ROM_SIZE / 512] = {0};
    HostRom source = {rom, trace_path ? sector_reads : NULL};
    Ws ws;
    if (!loaded || !ws_boot_model(&ws, (WsRom){read_rom, &source, (uint32_t)size}, color,
                                 bios_path ? bios : NULL)) {
        fprintf(stderr, "Could not load cartridge\n");
        free(rom);
        return 2;
    }
    if (bizhawk_timing) {
        ws.bizhawk_timing = true;
        ws.line = WS_HEIGHT + 1;
    }
    if (size == WS_FLASH_SIZE && memcmp(rom + 0x70000, "ELISA", 5) == 0 &&
        rom[0x7fff6] == 0 && rom[0x7fff8] == 0 && rom[0x7fffb] == 4 &&
        rom[0x7fffd] == 1)
        ws_attach_flash(&ws, store_flash, &source);

    uint8_t *sram = NULL;
    if (ws.cart_sram_size) {
        sram = calloc(1, ws.cart_sram_size);
        if (!ws_attach_sram(&ws, sram, ws.cart_sram_size)) {
            fprintf(stderr, "Could not allocate cartridge SRAM\n");
            free(rom);
            return 2;
        }
    }

    if (save_path && !load_nvram(save_path, &ws)) {
        fprintf(stderr, "Cannot load NVRAM (unreadable or wrong size): %s\n", save_path);
        free(sram);
        free(rom);
        return 2;
    }

    Wav wav = {.stereo = stereo};
    if (wav_path) {
        wav.file = fopen(wav_path, "wb");
        if (!wav.file) {
            perror(wav_path);
            free(sram);
            free(rom);
            return 2;
        }
        uint8_t blank[44] = {0};
        fwrite(blank, 1, sizeof(blank), wav.file);
        ws.audio.sink = record_sample;
        ws.audio.context = &wav;
    }

    Capture capture = {.frame = frame_limit, .color = color, .all_frames = frame_trace_path != NULL};
    FILE *input = input_path ? fopen(input_path, "r") : NULL;
    if (input_path && !input) {
        perror(input_path);
        if (wav.file)
            fclose(wav.file);
        free(sram);
        free(rom);
        return 2;
    }
    FILE *frame_trace = frame_trace_path ? fopen(frame_trace_path, "w") : NULL;
    if (frame_trace_path && !frame_trace) {
        perror(frame_trace_path);
        if (input)
            fclose(input);
        if (wav.file)
            fclose(wav.file);
        free(sram);
        free(rom);
        return 2;
    }
    if (frame_trace)
        fputs("frame,cs,ip,video_crc32\n", frame_trace);
    FILE *exec_trace = exec_trace_path ? fopen(exec_trace_path, "w") : NULL;
    if (exec_trace_path && !exec_trace) {
        perror(exec_trace_path);
        if (frame_trace) fclose(frame_trace);
        if (input) fclose(input);
        if (wav.file) fclose(wav.file);
        free(sram);
        free(rom);
        return 2;
    }
    if (exec_trace) {
        fputs("pc,rom_offset\n", exec_trace);
        ws.instruction_sink = trace_instruction;
        ws.instruction_context = exec_trace;
    }
    unsigned input_frame = 0;
    bool input_ok = true;
    uint64_t steps = 0;
    WsStep result = WS_STEP_OK;
    uint64_t step_limit = frame_limit ? frame_limit * UINT64_C(50000) : 1000000u;
    while (steps < step_limit && result == WS_STEP_OK &&
           (!frame_limit || ws.frames < frame_limit)) {
        if (input) {
            while (input_frame <= ws.frames) {
                unsigned keys;
                if (fscanf(input, "%x", &keys) != 1 || (keys & ~0xffeu)) {
                    fprintf(stderr, "Missing or invalid input at frame %u\n", input_frame + 1);
                    input_ok = false;
                    break;
                }
                ws_set_keys(&ws, keys);
                ++input_frame;
            }
            if (!input_ok)
                break;
        } else if (start_frame)
            ws_set_keys(&ws, ws.frames >= start_frame &&
                        ws.frames - start_frame < 3 ? 2 : 0);
        unsigned previous_frame = ws.frames;
        result = ws_step_with_video(&ws, frame_limit ? capture_line : NULL, &capture);
        ++steps;
        if (frame_trace && ws.frames != previous_frame)
            fprintf(frame_trace, "%u,%04x,%04x,%08x\n",
                    ws.frames, ws.cs, ws.ip, video_crc32(&capture));
    }
    if (input)
        fclose(input);
    bool frame_trace_ok = true;
    if (frame_trace) {
        frame_trace_ok = !ferror(frame_trace);
        frame_trace_ok &= fclose(frame_trace) == 0;
    }
    bool exec_trace_ok = true;
    if (exec_trace) {
        exec_trace_ok = !ferror(exec_trace);
        exec_trace_ok &= fclose(exec_trace) == 0;
    }
    if (result == WS_STEP_UNSUPPORTED)
        fprintf(stderr, "Unsupported opcode %02X at %04X:%04X (step %llu)\n",
                ws_read(&ws, ws_address(ws.cs, ws.ip)), ws.cs, ws.ip, (unsigned long long)steps);
    else if (frame_limit && ws.frames == frame_limit)
        printf("Completed %u frames after %llu steps at %04X:%04X\n",
               ws.frames, (unsigned long long)steps, ws.cs, ws.ip);
    else
        printf("%s after %llu steps at %04X:%04X AX=%04X\n",
               result == WS_STEP_HALTED ? "HLT" : "Step limit", (unsigned long long)steps,
               ws.cs, ws.ip, ws.reg[0]);

    if (wav.file && !finish_wav(&wav)) {
        fprintf(stderr, "Could not write %s\n", wav_path);
        free(sram);
        free(rom);
        return 2;
    }
    ws.audio.sink = NULL;
    bool nvram_saved = !save_path || !input_ok || result == WS_STEP_UNSUPPORTED ||
                       save_nvram(save_path, &ws);
    bool complete_frame = frame_limit && ws.frames == frame_limit;
    if (argc >= 3 && !complete_frame) {
        // Snapshot for instruction-limited or interrupted runs.
        ws.display[0x16] = 158;
        ws.line = bizhawk_timing ? WS_HEIGHT - 3 : WS_HEIGHT;
        ws.line_clock = 0;
        ws_video_clock(&ws, 256, NULL, NULL);
    }
    bool saved = argc == 2 || save_image(&ws, argv[2], complete_frame ? &capture : NULL, color);
    free(rom);
    free(sram);
    if (trace_path) {
        FILE *trace = fopen(trace_path, "w");
        if (!trace) {
            perror(trace_path);
            return 2;
        }
        for (unsigned sector = 0; sector < (unsigned)size / 512; ++sector)
            fprintf(trace, "%u\n", sector_reads[sector]);
        bool ok = !ferror(trace);
        if (fclose(trace) != 0 || !ok)
            return 2;
    }
    if (!nvram_saved) {
        fprintf(stderr, "Could not write %s\n", save_path);
        return 2;
    }
    if (!saved) {
        fprintf(stderr, "Could not write %s\n", argv[2]);
        return 2;
    }
    if (!frame_trace_ok) {
        fprintf(stderr, "Could not write %s\n", frame_trace_path);
        return 2;
    }
    if (!exec_trace_ok) {
        fprintf(stderr, "Could not write %s\n", exec_trace_path);
        return 2;
    }
    if (!input_ok)
        return 2;
    return result == WS_STEP_HALTED || complete_frame ? 0 : 1;
}
