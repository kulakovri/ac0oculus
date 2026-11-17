/* cpit_texdump.c -- one texture of a cockpit file (format 3 or 4) as a binary PPM (RGB) and a PGM (alpha): cpit_texdump IN.cpit INDEX
 * OUT_BASE [LEVEL]. Format 4 levels are decoded (BC7 / BC1, ps2_cpit4.h, the runtime's decoder); RGBA8 records give level 0
 * only (LEVEL > 0: 2 x 2 box mips as the packer and cpit_view build them). rq3_tex 2026-11-11 (N1.2, BUGS_RQ R-65): texture
 * crops of the Quest's compressed set against the source. Only reads IN.
 * gcc -O2 -Iruntime/include -o build/tools/cpit_texdump.exe tools/quest/cpit_texdump.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2_cpit4.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: cpit_texdump IN.cpit INDEX OUT_BASE [LEVEL]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    if (n < 20 || (memcmp(d, "ACZCPIT3", 8) && memcmp(d, "ACZCPIT4", 8))) { fprintf(stderr, "not a cockpit file\n"); return 1; }
    uint32_t ntex, want = (uint32_t)atoi(argv[2]), lev = argc > 4 ? (uint32_t)atoi(argv[4]) : 0;
    memcpy(&ntex, d + 8, 4);
    size_t p = 20;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t w, h, fm, fl; size_t tx, by;
        if (!cpit4_tex_rec(d, (size_t)n, p, &w, &h, &fm, &fl, &tx, &by)) { fprintf(stderr, "bad record %u\n", i); return 1; }
        if (i == want) {
            uint8_t *px; uint32_t lw = w, lh = h;
            if (fm == CPIT_TF_RGBA8) {
                px = (uint8_t *)malloc((size_t)w * h * 4); memcpy(px, d + tx, (size_t)w * h * 4);
                for (uint32_t l = 0; l < lev && (lw > 1 || lh > 1); l++) {
                    uint32_t nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
                    uint8_t *q = (uint8_t *)malloc((size_t)nw * nh * 4);
                    for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
                        uint32_t x0 = x * 2 < lw ? x * 2 : lw - 1, x1 = x * 2 + 1 < lw ? x * 2 + 1 : lw - 1;
                        uint32_t y0 = y * 2 < lh ? y * 2 : lh - 1, y1 = y * 2 + 1 < lh ? y * 2 + 1 : lh - 1;
                        q[(y * nw + x) * 4 + c] = (uint8_t)((px[(y0 * lw + x0) * 4 + c] + px[(y0 * lw + x1) * 4 + c]
                                                            + px[(y1 * lw + x0) * 4 + c] + px[(y1 * lw + x1) * 4 + c] + 2) / 4);
                    }
                    free(px); px = q; lw = nw; lh = nh;
                }
            } else {
                size_t o = tx;
                uint32_t nl = cpit4_levels(w, h);
                if (lev >= nl) lev = nl - 1;
                for (uint32_t l = 0; l < lev; l++) o += cpit4_level_bytes(fm, cpit4_lv_dim(w, l), cpit4_lv_dim(h, l));
                lw = cpit4_lv_dim(w, lev); lh = cpit4_lv_dim(h, lev);
                px = (uint8_t *)malloc((size_t)lw * lh * 4);
                cpit4_decode_level(fm, d + o, lw, lh, px);
            }
            char path[1024];
            snprintf(path, sizeof path, "%s.ppm", argv[3]);
            FILE *o = fopen(path, "wb"); if (!o) { perror(path); return 1; }
            fprintf(o, "P6\n%u %u\n255\n", lw, lh);
            for (size_t k = 0; k < (size_t)lw * lh; k++) fwrite(px + k * 4, 1, 3, o);
            fclose(o);
            snprintf(path, sizeof path, "%s_a.pgm", argv[3]);
            o = fopen(path, "wb"); if (!o) { perror(path); return 1; }
            fprintf(o, "P5\n%u %u\n255\n", lw, lh);
            for (size_t k = 0; k < (size_t)lw * lh; k++) fwrite(px + k * 4 + 3, 1, 1, o);
            fclose(o);
            printf("texture %u: %ux%u format %u flags %u; level %u: %ux%u\n", i, w, h, fm, fl, lev, lw, lh);
            return 0;
        }
        p = tx + by;
    }
    fprintf(stderr, "no texture %u (%u in the file)\n", want, ntex);
    return 1;
}
