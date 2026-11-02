/* deepeel_png.h: the text chunks of a deepeel map PNG (deepeel --map x.png).
 * The image is for people; the tool's data rides in zTXt chunks:
 *   deepeel_cov   the played coverage, in the .cov line format (hits from
 *                 played sessions only; proven-data runs as one line with a
 *                 5th column = run length)
 *   deepeel_rom   played ROM reads per 256-byte page, --rom-profile format
 *   deepeel_heat  played usage per memory-map zone, ranked (hot set)
 *   deepeel_map   the memory map (.map text), deepeel_meta its settings
 * No image decoding: only the chunk walk (CRC checked) and zlib. */
#pragma once

#include <filesystem>
#include <string>

namespace m0boy::deepeel {

/* true when the file starts with the PNG signature */
bool is_png(const std::filesystem::path& path);

/* text of the tEXt/zTXt chunk `key`; false when absent or unreadable */
bool png_text(const std::filesystem::path& path, const std::string& key, std::string& out);

/* coverage lines of a --coverage input: the file itself (.cov text) or, for a
 * deepeel map PNG, its deepeel_cov chunk.  `error` is set on failure. */
bool coverage_text(const std::filesystem::path& path, std::string& out, std::string& error);

} // namespace m0boy::deepeel
