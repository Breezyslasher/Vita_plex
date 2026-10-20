#include "utils/cover_png.hpp"

#include <algorithm>

namespace vitaplex {
namespace cover {

std::vector<uint8_t> squareRgb(const uint8_t* rgb, int width, int height, int size) {
    std::vector<uint8_t> out((size_t)size * size * 3);
    const int side = std::min(width, height);
    const int left = (width - side) / 2, top = (height - side) / 2;
    for (int dy = 0; dy < size; dy++) {
        const int y0 = top + dy * side / size;
        const int y1 = std::max(top + (dy + 1) * side / size, y0 + 1);
        for (int dx = 0; dx < size; dx++) {
            const int x0 = left + dx * side / size;
            const int x1 = std::max(left + (dx + 1) * side / size, x0 + 1);
            uint32_t sum[3] = {0, 0, 0};
            for (int y = y0; y < y1; y++) {
                const uint8_t* p = rgb + ((size_t)y * width + x0) * 3;
                for (int x = x0; x < x1; x++, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const uint32_t count = (uint32_t)(y1 - y0) * (uint32_t)(x1 - x0);
            uint8_t* o = &out[((size_t)dy * size + dx) * 3];
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)((sum[c] + count / 2) / count);
        }
    }
    return out;
}

namespace {

void put32(std::string& s, uint32_t v) {
    s += (char)(v >> 24);
    s += (char)(v >> 16);
    s += (char)(v >> 8);
    s += (char)v;
}

uint32_t crc32(const std::string& s, size_t from) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = from; i < s.size(); i++) {
        c ^= (uint8_t)s[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// A chunk: its length, type and data, then the CRC of type and data.
void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (uint32_t)data.size());
    const size_t from = png.size();
    png += type;
    png += data;
    put32(png, crc32(png, from));
}

}  // namespace

std::string pngRgb(const std::vector<uint8_t>& rgb, int size) {
    // The rows, each after its filter type: 0, none.
    std::string raw;
    raw.reserve((size_t)size * (1 + (size_t)size * 3));
    for (int y = 0; y < size; y++) {
        raw += '\0';
        raw.append((const char*)&rgb[(size_t)y * size * 3], (size_t)size * 3);
    }
    // A zlib stream (deflate, no dictionary) of stored blocks, 65535 bytes at
    // most each, then the rows' Adler-32.
    std::string z("\x78\x01", 2);
    for (size_t at = 0;;) {
        const size_t len = std::min<size_t>(raw.size() - at, 65535);
        const bool last = at + len == raw.size();
        z += (char)(last ? 1 : 0);
        z += (char)(len & 0xFF);
        z += (char)(len >> 8);
        z += (char)(~len & 0xFF);
        z += (char)((~len >> 8) & 0xFF);
        z.append(raw, at, len);
        at += len;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (char c : raw) {
        a = (a + (uint8_t)c) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::string header;
    put32(header, (uint32_t)size);
    put32(header, (uint32_t)size);
    header += '\x08';  // bits a channel
    header += '\x02';  // RGB
    header += std::string(3, '\0');  // deflate, adaptive filters, not interlaced
    std::string png("\x89PNG\r\n\x1a\n", 8);
    chunk(png, "IHDR", header);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", std::string());
    return png;
}

}  // namespace cover
}  // namespace vitaplex
