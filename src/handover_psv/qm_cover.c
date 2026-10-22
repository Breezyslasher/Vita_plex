/**
 * The song's cover for VitaPlex's quick menu plugin: qm_cover.h. stb_image
 * comes with nanovg in VitaPlex (borealis).
 */

#include "qm_cover.h"

#include "qm_protocol.h"

#include "stb_image.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void qmCoverSquare(const uint8_t* rgb, int width, int height, int size, uint8_t* out) {
    const int side = width < height ? width : height;
    const int left = (width - side) / 2, top = (height - side) / 2;
    for (int dy = 0; dy < size; dy++) {
        const int y0 = top + dy * side / size;
        int y1 = top + (dy + 1) * side / size;
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < size; dx++) {
            const int x0 = left + dx * side / size;
            int x1 = left + (dx + 1) * side / size;
            if (x1 <= x0) x1 = x0 + 1;
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
            uint8_t* o = out + ((size_t)dy * size + dx) * 3;
            for (int c = 0; c < 3; c++) o[c] = (uint8_t)((sum[c] + count / 2) / count);
        }
    }
}

static uint8_t* put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return p + 4;
}

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// A chunk into p: its length, type and data, then the CRC of type and data.
// Past it.
static uint8_t* chunk(uint8_t* p, const char* type, const uint8_t* data, size_t len) {
    p = put32(p, (uint32_t)len);
    uint8_t* from = p;
    memcpy(p, type, 4);
    p += 4;
    if (len) memcpy(p, data, len);
    p += len;
    return put32(p, crc32(from, (size_t)(p - from)));
}

uint8_t* qmCoverPng(const uint8_t* rgb, int size, size_t* len) {
    // The rows, each after its filter type: 0, none.
    const size_t row = 1 + (size_t)size * 3, raw = (size_t)size * row;
    uint8_t* rows = (uint8_t*)malloc(raw ? raw : 1);
    if (!rows) return NULL;
    for (int y = 0; y < size; y++) {
        rows[(size_t)y * row] = 0;
        memcpy(rows + (size_t)y * row + 1, rgb + (size_t)y * size * 3, (size_t)size * 3);
    }
    const size_t blocks = raw ? (raw + 65534) / 65535 : 1;
    const size_t zlen = 2 + blocks * 5 + raw + 4;
    uint8_t* png = (uint8_t*)malloc(8 + 25 + 12 + zlen + 12);
    if (!png) {
        free(rows);
        return NULL;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint8_t* p = png;
    memcpy(p, signature, 8);
    p += 8;
    uint8_t header[13];
    put32(header, (uint32_t)size);
    put32(header + 4, (uint32_t)size);
    header[8] = 8;   // bits a channel
    header[9] = 2;   // RGB
    header[10] = 0;  // deflate
    header[11] = 0;  // adaptive filters
    header[12] = 0;  // not interlaced
    p = chunk(p, "IHDR", header, sizeof(header));
    // A zlib stream (deflate, no dictionary) of stored blocks, 65535 bytes at
    // most each, then the rows' Adler-32.
    p = put32(p, (uint32_t)zlen);
    uint8_t* from = p;
    memcpy(p, "IDAT", 4);
    p += 4;
    *p++ = 0x78;
    *p++ = 0x01;
    for (size_t at = 0;;) {
        const size_t n = raw - at < 65535 ? raw - at : 65535;
        const int last = at + n == raw;
        *p++ = (uint8_t)(last ? 1 : 0);
        *p++ = (uint8_t)(n & 0xFF);
        *p++ = (uint8_t)(n >> 8);
        *p++ = (uint8_t)(~n & 0xFF);
        *p++ = (uint8_t)((~n >> 8) & 0xFF);
        memcpy(p, rows + at, n);
        p += n;
        at += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) {
        a = (a + rows[i]) % 65521;
        b = (b + a) % 65521;
    }
    free(rows);
    p = put32(p, (b << 16) | a);
    p = put32(p, crc32(from, (size_t)(p - from)));
    p = chunk(p, "IEND", NULL, 0);
    *len = (size_t)(p - png);
    return png;
}

uint32_t qmCoverNextSerial(uint32_t last, uint64_t nowUs) {
    uint32_t serial = (uint32_t)(nowUs / 1000);
    if (serial <= last) serial = last + 1;
    if (last != 0 && (serial & 1) == (last & 1)) serial++;
    return serial ? serial : 1;
}

// An image as a cover PNG at path. 0 when written; else -1, with what failed
// in why.
static int writeCover(const void* image, size_t len, const char* path, char* why,
                      size_t whySize) {
    if (len > INT_MAX) {
        snprintf(why, whySize, "too big (%u bytes)", (unsigned)len);
        return -1;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc* px =
        stbi_load_from_memory((const stbi_uc*)image, (int)len, &width, &height, &channels, 3);
    if (!px) {
        const char* reason = stbi_failure_reason();
        snprintf(why, whySize, "not decoded (%u bytes): %s", (unsigned)len, reason ? reason : "?");
        return -1;
    }
    uint8_t* square = (uint8_t*)malloc((size_t)QM_ART_SIZE * QM_ART_SIZE * 3);
    uint8_t* png = NULL;
    size_t pngLen = 0;
    if (square) {
        qmCoverSquare(px, width, height, QM_ART_SIZE, square);
        png = qmCoverPng(square, QM_ART_SIZE, &pngLen);
    }
    stbi_image_free(px);
    free(square);
    if (!png) {
        snprintf(why, whySize, "no memory for a %dx%d image", width, height);
        return -1;
    }
    FILE* f = fopen(path, "wb");
    if (!f) {
        free(png);
        snprintf(why, whySize, "not written: %s would not open", path);
        return -1;
    }
    const int wrote = fwrite(png, 1, pngLen, f) == pngLen;
    const int closed = fclose(f) == 0;
    free(png);
    if (!wrote || !closed) {
        snprintf(why, whySize, "not written to %s", path);
        return -1;
    }
    return 0;
}

int qmCoverWrite(const void* image, size_t len, uint32_t serial, char* why, size_t whySize) {
    char path[64];
    snprintf(path, sizeof(path), QM_ART_PATH_FMT, (unsigned)(serial & 1));
    return writeCover(image, len, path, why, whySize);
}
