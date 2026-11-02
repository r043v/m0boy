/* deepeel_png.cpp: text chunks of a deepeel map PNG (see deepeel_png.h). */
#include "recompiler/codegen/deepeel_png.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <zlib.h>

namespace m0boy::deepeel {

namespace {

const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool read_all(const std::filesystem::path& path, std::vector<unsigned char>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool inflate_all(const unsigned char* src, size_t n, std::string& out) {
    z_stream s{};
    if (inflateInit(&s) != Z_OK) return false;
    s.next_in = const_cast<Bytef*>(src);
    s.avail_in = static_cast<uInt>(n);
    out.clear();
    unsigned char buf[65536];
    int r;
    do {
        s.next_out = buf;
        s.avail_out = sizeof buf;
        r = inflate(&s, Z_NO_FLUSH);
        out.append(reinterpret_cast<char*>(buf), sizeof buf - s.avail_out);
    } while (r == Z_OK);
    inflateEnd(&s);
    return r == Z_STREAM_END;
}

} // namespace

bool is_png(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char sig[8];
    return in.read(reinterpret_cast<char*>(sig), 8) && !std::memcmp(sig, kSig, 8);
}

bool png_text(const std::filesystem::path& path, const std::string& key, std::string& out) {
    std::vector<unsigned char> d;
    if (!read_all(path, d) || d.size() < 8 || std::memcmp(d.data(), kSig, 8)) return false;
    size_t i = 8;
    while (i + 12 <= d.size()) {
        const uint32_t len = be32(&d[i]);
        if (i + 12 + size_t(len) > d.size()) return false;
        const unsigned char* type = &d[i + 4];
        const unsigned char* body = &d[i + 8];
        uLong crc = crc32(0L, type, 4);
        crc = crc32(crc, body, len);
        if (crc != be32(body + len)) return false; /* corrupt: stop */
        if (!std::memcmp(type, "IEND", 4)) break;
        const bool text = !std::memcmp(type, "tEXt", 4), ztext = !std::memcmp(type, "zTXt", 4);
        if ((text || ztext) && len > key.size() && !std::memcmp(body, key.data(), key.size()) &&
            body[key.size()] == 0) {
            const unsigned char* p = body + key.size() + 1;
            const size_t n = len - key.size() - 1;
            if (text) {
                out.assign(reinterpret_cast<const char*>(p), n);
                return true;
            }
            /* zTXt: compression method byte (0 = deflate), then zlib */
            return n > 1 && p[0] == 0 && inflate_all(p + 1, n - 1, out);
        }
        i += 12 + size_t(len);
    }
    return false;
}

bool coverage_text(const std::filesystem::path& path, std::string& out, std::string& error) {
    if (is_png(path)) {
        if (png_text(path, "deepeel_cov", out)) return true;
        error = "no deepeel_cov chunk in " + path.string() + " (not a deepeel map PNG?)";
        return false;
    }
    std::vector<unsigned char> d;
    if (!read_all(path, d)) {
        error = "cannot read coverage file " + path.string();
        return false;
    }
    out.assign(d.begin(), d.end());
    return true;
}

} // namespace m0boy::deepeel
