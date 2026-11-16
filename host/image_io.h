// Tiny image writers for screenshots: PNG (stored deflate blocks, no dependency) and 24-bit BMP (the engine's F8 format).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <vector>

inline uint32_t Crc32(const uint8_t *p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; table[i] = c; } init = true; }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}
inline bool WritePng(const char *path, uint32_t w, uint32_t h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    auto be = [](uint8_t *o, uint32_t v) { o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = v; };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        uint8_t len[4]; be(len, (uint32_t)data.size()); fwrite(len, 1, 4, f);
        std::vector<uint8_t> body(type, type + 4); body.insert(body.end(), data.begin(), data.end());
        fwrite(body.data(), 1, body.size(), f);
        uint8_t crc[4]; be(crc, Crc32(body.data(), body.size())); fwrite(crc, 1, 4, f);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr(13); be(&ihdr[0], w); be(&ihdr[4], h); ihdr[8] = 8; ihdr[9] = 2;
    chunk("IHDR", ihdr);
    size_t stride = (size_t)w * 3 + 1;
    std::vector<uint8_t> raw(stride * h);
    for (uint32_t y = 0; y < h; y++) { raw[y * stride] = 0; memcpy(&raw[y * stride + 1], rgb + (size_t)y * w * 3, (size_t)w * 3); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t off = 0; off < raw.size();) {
        size_t n = raw.size() - off; if (n > 65535) n = 65535;
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back(n & 255); z.push_back((uint8_t)(n >> 8)); z.push_back(~n & 255); z.push_back((uint8_t)((~n >> 8) & 255));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        for (size_t i = 0; i < n; i++) { a = (a + raw[off + i]) % 65521; b = (b + a) % 65521; }
        off += n;
    }
    uint8_t ad[4]; be(ad, (b << 16) | a); z.insert(z.end(), ad, ad + 4);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
    return true;
}
inline bool WriteBmp(const char *path, uint32_t w, uint32_t h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t stride = (w * 3 + 3) & ~3u, bytes = 54 + stride * h;
    std::vector<uint8_t> bmp(bytes, 0);
    auto d = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) bmp[at + i] = (uint8_t)(v >> (i * 8)); };
    bmp[0] = 'B'; bmp[1] = 'M'; d(2, bytes); d(10, 54); d(14, 40); d(18, w); d(22, h); bmp[26] = 1; bmp[28] = 24; d(34, stride * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = rgb + ((size_t)(h - 1 - y) * w + x) * 3;
            uint8_t *o = &bmp[54 + (size_t)stride * y + x * 3];
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0];
        }
    fwrite(bmp.data(), 1, bmp.size(), f);
    fclose(f);
    return true;
}
