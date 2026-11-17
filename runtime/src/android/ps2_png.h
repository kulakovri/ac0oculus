/* ps2_png.h -- the PNG reader of the Quest build's SDL subset (ps2_sdl_android.c: SDL_LoadPNG), in a header of its own so
   the same code is tested on the PC against SDL3's own SDL_LoadPNG (tools/quest/png_test.c). Pure C + miniz's inflate.
   8-bit gray / RGB / palette (1-8 bit) / gray+alpha / RGBA, 16-bit samples by their high byte, tRNS (palette alpha and the
   gray / RGB colour key), Adam7 interlace; up to 65536 px a side and 64 Mpx.
   Output: malloc'ed RGBA rows (w * 4 bytes each), or NULL with a reason in err. */
#ifndef PS2_PNG_H
#define PS2_PNG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

static uint32_t ps2_png_be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int ps2_png_paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
static unsigned char *ps2_png_decode(const unsigned char *d, size_t n, uint32_t *ow, uint32_t *oh, char *err, size_t errcap) {
    unsigned char *idat = NULL, pal[256 * 4], *px = NULL;
    size_t ni = 0, ci = 0;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, inter = 0, npal = 0, has_key = 0;
    unsigned key[3] = { 0, 0, 0 };                   /* tRNS of gray / RGB images: the transparent sample value(s) */
    memset(pal, 255, sizeof pal);
    *ow = *oh = 0;
    if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) { snprintf(err, errcap, "not a PNG"); return NULL; }
    for (size_t o = 8; o + 12 <= n;) {
        uint32_t len = ps2_png_be32(d + o);
        const unsigned char *t = d + o + 4, *c = d + o + 8;
        if (o + 12 + (size_t)len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = ps2_png_be32(c); h = ps2_png_be32(c + 4); depth = c[8]; ctype = c[9]; inter = c[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = (int)(len / 3u);
            for (int i = 0; i < npal && i < 256; i++) { pal[i * 4] = c[i * 3]; pal[i * 4 + 1] = c[i * 3 + 1]; pal[i * 4 + 2] = c[i * 3 + 2]; }
        } else if (!memcmp(t, "tRNS", 4) && ctype == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++) pal[i * 4 + 3] = c[i];
        } else if (!memcmp(t, "tRNS", 4) && ((ctype == 0 && len >= 2) || (ctype == 2 && len >= 6))) {
            has_key = 1;
            for (int k = 0; k < (ctype == 0 ? 1 : 3); k++) key[k] = (unsigned)c[k * 2] << 8 | c[k * 2 + 1];
        } else if (!memcmp(t, "IDAT", 4)) {
            if (ni + len > ci) {
                ci = (ni + len) * 2;
                unsigned char *g = (unsigned char *)realloc(idat, ci);
                if (!g) { free(idat); snprintf(err, errcap, "out of memory"); return NULL; }
                idat = g;
            }
            memcpy(idat + ni, c, len);
            ni += len;
        } else if (!memcmp(t, "IEND", 4)) break;
        o += 12 + (size_t)len;
    }
    if (!w || !h || inter > 1 || ctype < 0 || !idat || w > 65536 || h > 65536 || (uint64_t)w * h > (64u << 20)) {
        snprintf(err, errcap, "unsupported PNG (%ux%u, interlace %d, type %d)", w, h, inter, ctype);
        free(idat);
        return NULL;
    }
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!chans || (depth != 8 && depth != 16 && !(depth < 8 && (ctype == 3 || ctype == 0)))) {
        snprintf(err, errcap, "unsupported PNG type %d/%d", ctype, depth);
        free(idat);
        return NULL;
    }
    size_t bpp_bits = (size_t)chans * (size_t)depth, bpp = (bpp_bits + 7) / 8, raw_n = 0;
    unsigned char *raw = (unsigned char *)tinfl_decompress_mem_to_heap(idat, ni, &raw_n, TINFL_FLAG_PARSE_ZLIB_HEADER);
    free(idat);
    if (!raw) { snprintf(err, errcap, "bad image data"); return NULL; }
    px = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    unsigned char *prev = (unsigned char *)malloc(((size_t)w * bpp_bits + 7) / 8 + 1), *cur = (unsigned char *)malloc(((size_t)w * bpp_bits + 7) / 8 + 1);
    /* the passes: one whole image, or Adam7's seven reduced images (x0, y0, dx, dy), each with its own filtered rows */
    static const int adam7[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 },
                                     { 0, 1, 1, 2 } };
    static const int whole[1][4] = { { 0, 0, 1, 1 } };
    const int (*pass)[4] = inter ? adam7 : whole;
    int npass = inter ? 7 : 1;
    size_t off = 0;
    int bad = !px || !prev || !cur;
    for (int ps = 0; ps < npass && !bad; ps++) {
        uint32_t x0 = (uint32_t)pass[ps][0], y0 = (uint32_t)pass[ps][1], dx = (uint32_t)pass[ps][2], dy = (uint32_t)pass[ps][3];
        uint32_t pw = w > x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > y0 ? (h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        size_t stride = ((size_t)pw * bpp_bits + 7) / 8;
        if (off + (stride + 1) * (size_t)ph > raw_n) { bad = 1; break; }
        memset(prev, 0, stride);
        for (uint32_t y = 0; y < ph; y++) {
            const unsigned char *src = raw + off + (stride + 1) * (size_t)y;
            int f = src[0];
            src++;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, v = src[i];
                cur[i] = (unsigned char)(f == 1 ? v + a : f == 2 ? v + b : f == 3 ? v + ((a + b) >> 1)
                                         : f == 4 ? v + ps2_png_paeth(a, b, c) : v);
            }
            for (uint32_t x = 0; x < pw; x++) {
                unsigned char *o = px + ((size_t)(y0 + y * dy) * w + x0 + x * dx) * 4u;
                unsigned s[4] = { 0, 0, 0, 255 };
                if (depth < 8) {
                    unsigned bit = x * (unsigned)depth, v = (cur[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1u);
                    if (ctype == 3) { memcpy(o, pal + v * 4, 4); continue; }
                    if (has_key && v == key[0]) s[3] = 0;
                    v = v * 255u / ((1u << depth) - 1u);
                    s[0] = s[1] = s[2] = v;
                } else {
                    size_t step = depth == 16 ? 2 : 1;
                    const unsigned char *q = cur + (size_t)x * (size_t)chans * step;
                    for (int k = 0; k < chans; k++) s[k] = q[(size_t)k * step];
                    if (ctype == 3) { memcpy(o, pal + s[0] * 4, 4); continue; }
                    int keyed = 0;
                    if (has_key) {                   /* (the key compares the full sample, 16-bit included) */
                        keyed = 1;
                        for (int k = 0; k < chans; k++) {
                            unsigned full = step == 2 ? (unsigned)q[(size_t)k * 2] << 8 | q[(size_t)k * 2 + 1] : q[k];
                            if (full != key[k]) keyed = 0;
                        }
                    }
                    if (ctype == 0) { s[1] = s[2] = s[0]; }
                    else if (ctype == 4) { s[3] = s[1]; s[1] = s[2] = s[0]; }
                    else if (ctype == 2) s[3] = 255;
                    if (keyed) s[3] = 0;
                }
                o[0] = (unsigned char)s[0]; o[1] = (unsigned char)s[1]; o[2] = (unsigned char)s[2]; o[3] = (unsigned char)s[3];
            }
            unsigned char *tmp = prev; prev = cur; cur = tmp;
        }
        off += (stride + 1) * (size_t)ph;
    }
    if (bad && px) { free(px); px = NULL; free(prev); free(cur); free(raw); snprintf(err, errcap, "bad image data"); return NULL; }
    free(prev); free(cur); free(raw);
    if (!px) { snprintf(err, errcap, "out of memory"); return NULL; }
    *ow = w; *oh = h;
    return px;
}

#endif
