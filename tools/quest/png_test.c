/* png_test.c -- the Quest build's PNG reader (runtime/src/android/ps2_png.h, the SDL subset's SDL_LoadPNG) against SDL3's
   own SDL_LoadPNG + SDL_ConvertSurface(RGBA32), on the PC, over every file named on the command line: the same pixels or
   the first difference. Build and run (Git Bash, MSYS2's gcc and SDL3):
     export PATH=/c/msys64/ucrt64/bin:$PATH
     gcc -O2 -o build/png_test.exe tools/quest/png_test.c third_party/miniz/miniz.c -Iruntime/src/android -Ithird_party/miniz \
         -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME $(pkg-config --cflags --libs sdl3)
     find textures -iname '*.png' -print0 | xargs -0 build/png_test.exe
   Exit 0 = every file identical. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps2_png.h"

int main(int argc, char **argv) {
    int same = 0, differ = 0, both_fail = 0, ours_fail = 0, sdl_fail = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("cannot open %s\n", argv[i]); ours_fail++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = (unsigned char *)malloc((size_t)n);
        if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); ours_fail++; continue; }
        fclose(f);
        uint32_t w = 0, h = 0;
        char why[160];
        unsigned char *px = ps2_png_decode(d, (size_t)n, &w, &h, why, sizeof why);
        free(d);
        SDL_Surface *s = SDL_LoadPNG(argv[i]), *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
        if (!px && !c) { both_fail++; printf("both fail: %s (ours: %s)\n", argv[i], why); }
        else if (!px) { ours_fail++; printf("OURS FAIL: %s: %s (SDL reads it, %dx%d)\n", argv[i], why, c->w, c->h); }
        else if (!c) { sdl_fail++; printf("sdl fails, ours reads: %s (%ux%u)\n", argv[i], w, h); }
        else if ((int)w != c->w || (int)h != c->h) { differ++; printf("SIZE: %s ours %ux%u sdl %dx%d\n", argv[i], w, h, c->w, c->h); }
        else {
            int bad = -1;
            for (uint32_t y = 0; y < h && bad < 0; y++)
                if (memcmp(px + (size_t)y * w * 4u, (unsigned char *)c->pixels + (size_t)y * (size_t)c->pitch, (size_t)w * 4u)) bad = (int)y;
            if (bad < 0) same++;
            else {
                differ++;
                const unsigned char *a = px + (size_t)bad * w * 4u, *b = (unsigned char *)c->pixels + (size_t)bad * (size_t)c->pitch;
                uint32_t x = 0;
                while (x < w && !memcmp(a + x * 4u, b + x * 4u, 4)) x++;
                printf("DIFFER: %s (%ux%u) first at x %u y %d: ours %02x%02x%02x%02x sdl %02x%02x%02x%02x\n", argv[i], w, h, x, bad,
                       a[x * 4], a[x * 4 + 1], a[x * 4 + 2], a[x * 4 + 3], b[x * 4], b[x * 4 + 1], b[x * 4 + 2], b[x * 4 + 3]);
            }
        }
        free(px);
        SDL_DestroySurface(c);
        SDL_DestroySurface(s);
    }
    printf("png_test: %d files: %d identical, %d differ, ours fails %d, SDL fails %d, both fail %d\n", argc - 1, same, differ,
           ours_fail, sdl_fail, both_fail);
    return differ || ours_fail ? 1 : 0;
}
