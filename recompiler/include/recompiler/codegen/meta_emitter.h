/**
 * @file meta_emitter.h
 * @brief Compact coverage-driven code generation for the gbl runtime
 *        (Gamebuino META and other small microcontrollers).
 *
 * Unlike the desktop emitter, this backend compiles only instructions that a
 * profiling run actually executed (see targets/meta/host), relies on the gbl
 * interpreter for everything else, and trades per-instruction timing for
 * block-level cycle accounting so the generated code fits in on-chip flash.
 */

#ifndef RECOMPILER_CODEGEN_META_EMITTER_H
#define RECOMPILER_CODEGEN_META_EMITTER_H

#include "recompiler/rom.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gbrecomp {

struct MetaTargetOptions {
    std::string name;                                  ///< C identifier for files
    /* 8.3 base of the game's files on the SD card (NAME.BIN, NAME.GB,
     * NAME.SAV, NAME.STA); empty: derived from name. */
    std::string sd_name;
    std::filesystem::path output_dir;
    std::filesystem::path template_dir;                ///< targets/meta
    /* Console: the Gamebuino META (PlatformIO, Arduino core) or the Pokitto
     * (bare-metal Makefile; same runtime and generated code, its own
     * drivers in platform/pokitto, 228 KiB of flash below its loader). */
    bool pokitto = false;
    bool espboy = false; ///< ESP8266, 1 MiB flash, 256 KiB firmware, no SD
    bool pc = false;     ///< SDL2 window, unlimited flash, the card as sd.img
    bool odroid = false; ///< ODROID-GO: ESP32, ILI9341 320x240, SD, 16 MiB flash
    bool aka = false;    ///< Gamebuino AKA: ESP32-S3, 320x240, SD, 8 MiB PSRAM
    /// the ESP32 family shares its core (targets/common/consoles/esp32)
    bool esp32_console() const { return odroid || aka; }
    std::vector<std::filesystem::path> coverage_files; ///< gbl coverage v1 files
    bool emit_comments = true;
    std::string compiler = "gcc"; ///< firmware C/C++ compiler: gcc or clang
    bool thumb = false;        ///< experimental register-IR / Thumb-1 lowering
    bool precise = false;      ///< poll interrupts before every instruction
    size_t max_region_instructions = 256;
    /* Flash available for generated code; 0 = derive from the application
     * area (META 240 KiB, Pokitto 228 KiB) minus the runtime (~40 KiB) and
     * the ROM image.
     * Played coverage is always compiled; speculative (static analysis)
     * instructions fill the remaining budget, loops first, then bank 0. */
    size_t code_budget_bytes = 0;
    size_t bytes_per_instruction = 20;
    /* ROM streaming from the SD card: -1 automatic (ROMs of 256 KiB or more,
     * which cannot fit in flash), 0 off, 1 on.  Bank 0 and the most-read
     * 4 KiB chunks of the other banks stay in flash (the resident set), the
     * rest is read from <stream name>.GB on the card through a small cache. */
    int stream = -1;
    std::vector<std::filesystem::path> rom_profile_files; ///< host --rom-profile-out
    std::filesystem::path rom_plan; ///< m0romsim's ranking (tools/profile): SD reads' worth saved per block, per frame
    size_t rom_resident_bytes = 0; ///< 0 = automatic (at most half the free flash)
    /* Streamed part of the ROM kept in flash, LZSS-compressed per 512-byte
     * sector, instead of on the SD card (GBL_ROM_PACKED).  Falls back to SD
     * streaming (ROMs of 256 KiB or more) or to a plain build when the
     * compressed ROM leaves too little flash for code. */
    bool pack = false;
    /* --meta-sweep: every bank's CALL ($CD) and JP abs ($C3) offers its
     * target as a speculative entry (m0boy/sweep.h), so the callers
     * behind a bank table compile without a play session having run
     * there; the budget ranks them with the static analysis's candidates */
    bool sweep = false;
};

struct MetaTargetStats {
    size_t covered = 0;       ///< coverage records read
    size_t compiled = 0;      ///< instructions emitted as C
    size_t dropped = 0;       ///< covered but left to the interpreter
    size_t regions = 0;
    size_t dispatch_entries = 0;
    size_t register_blocks = 0; ///< blocks simplified by register IR
    size_t thumb_blocks = 0;    ///< register blocks with native Thumb lowering
    size_t speculative = 0;      ///< static-analysis instructions offered
    size_t speculative_kept = 0; ///< ... and compiled within the budget
    size_t data_skipped = 0;     ///< cov lines flagged data (8) and not executed
    size_t budget_instructions = 0;
    bool stream = false;         ///< ROM streamed from the SD card
    std::string stream_file;     ///< its 8.3 name on the card
    size_t resident_chunks = 0;  ///< 4 KiB chunks kept in flash (bank 0 excluded)
    size_t streamed_chunks = 0;  ///< chunks read from the card (or packed)
    bool packed = false;         ///< streamed chunks compressed in flash
    bool sd = false;             ///< at least some ROM data may come from the card
    size_t packed_bytes = 0;     ///< their compressed size
    size_t packed_raw_bytes = 0; ///< ... and their size before compression
    std::string pack_note;       ///< why --meta-pack was not applied
    size_t spin_loops = 0;       ///< polling loops fast-forwarded to the next change
    size_t counted_loops = 0;    ///< DEC/JR NZ delay loops fast-forwarded
};

/**
 * @brief Write a firmware (PlatformIO for the META, Makefile for the Pokitto)
 *        + host-harness project for the gbl runtime.
 * @return false and fill @p error when generation is impossible.
 */
bool generate_meta_project(const ROM& rom, const MetaTargetOptions& options,
                           MetaTargetStats& stats, std::string& error);

} // namespace gbrecomp

#endif
