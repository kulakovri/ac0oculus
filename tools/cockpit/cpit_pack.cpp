// cpit_pack.cpp -- smaller cockpit files: an ACZCPIT3 file (tools/cockpit/ac2cpit.py) to its cleaned ACZCPIT3 or to
// ACZCPIT4 (runtime/include/ps2_cpit4.h). night_assets 2026-11-04, RELEASE_RESEARCH_2026-11-03.md sections 3 and 15 (R1).
//
//   cpit_pack IN.cpit OUT.cpit [options]
//     --format 3|4    3: only the clean-up below, a file today's loader reads; 4 (default): indexed meshes + --tex
//     --tex bc7|raw   format 4's textures: BC7 with a stored mip chain (default), or RGBA8 level 0 as format 3
//     --bc1 REGEX     (format 4) opaque textures used only by meshes whose names match REGEX go to BC1 (needs an A/B, see
//                     NOTES; off by default)
//     --q16           (format 4) 16-byte vertices where the loss stays small (position grid 0.05 mm, UV under 1/16
//                     texel, unit normals, u8 AO); not exact, so off by default
//     --no-clean      keep every texture as it is (no dedupe, no drop, no solids)
//     --uber N        the BC7 encoder's search effort, 0..4 (default 1)
//     --raw-below DB  a texture whose BC7 level 0 stays under DB dB PSNR (RGB) stays RGBA8 (format word per texture): small
//                     sharp-edged label atlases the encoder's modes 1, 5, 6, 7 cannot hold; default 0 = none
//     --raw-max-bytes N  ... only if its RGBA8 level 0 takes at most N bytes (default no limit)
//     --raw-text DB,P8,N  (rq3_tex, the Quest set) also RGBA8: a texture of at most N bytes as RGBA8 level 0 whose BC level 0
//                     holds under DB dB or more than P8 % of its texels off by more than 8 of 255 -- the small atlases of text,
//                     dials, counter drums and glyphs (the F-14A's drum 44.6 dB, 0.9 %; the J35J's gauges 44.1 dB, 1.5 %)
//     --threads N     encoder threads (default NUMBER_OF_PROCESSORS)
//     --report F.tsv  per texture: size, format, flags, bytes, PSNR RGB / A (level 0), max channel error, alpha-cut flips
//     --no-verify     skip reading the output back (by default it is expanded and compared with the input)
//
//   The Quest set (rq3_tex 2026-11-11, PROMPT_2026-11-11_night_cockpits N1.2; all off by default, the PC's files as before):
//     --need PX       size each texture by need: its stored chain starts at the finest mip level the sampler can ever take
//                     for it from the eye box (the levels above are never read, so leaving them out is exact for the eye).
//                     PX = the eye image's pixels per tangent at its centre (the Quest 3 at PS2_XR_EYE_SCALE 1.0: 1680 px
//                     over tan 54 + tan 40 deg = 758). Per triangle: the texture map's Jacobian (texels per metre in the
//                     triangle's plane, singular values s1 >= s2), the nearest approach d of the triangle to the eye box
//                     (the distance from the design eye minus --need-lean, minus --need-move more for animated parts, at
//                     least --need-near), and the sampler's anisotropic level (Vulkan: lambda = log2(max(rho_min,
//                     rho_max / aniso)) with rho the footprint in texels along the pixel's x / y) bounded from below by
//                     its value face-on at d: log2(max(s2, s1 / (sqrt 2 x aniso)) x d / PX'), PX' = PX x --need-margin.
//                     A texture whose least lambda over all its triangles is L >= 1 is stored from level floor(L). A
//                     triangle whose texture coordinates collapse to a point (lambda -inf: magnified, one colour: a palette
//                     atlas) shows level 0's bilinear texel at that point: the drop stands only where level floor(L)
//                     gives every such point the same colour within --need-point-tol (default 2 of 255), else a level
//                     less is left out, down to none.
//     --need-margin F (default 1.5) density margin over the image's centre: a planar eye image has 1 / cos^2 more pixels per
//                     radian off its axis (1.5 = 35 deg off axis, 2.0 = 45 deg; beyond, the Quest's foveation halves the
//                     density), and the GPU's own rounding of lambda. The Quest set (tools/quest/make_cockpits_q4.sh) uses 2.0
//                     with --need-lean 0.25: the least margin at which cpit_view -texlod's audit and head-box views (leaning up
//                     to 23 cm, the image to 54 deg) never read a level the file leaves out (F-117: 1.5 / 0.15 had 5 textures
//                     read up to 0.24 level below their stored top; 2.0 / 0.25 none, 0.33 level to spare)
//     --need-lean M   (0.15) the head's reach from the design eye; --need-near M (0.20) the nearest distance judged;
//     --need-move M   (0.10) the extra reach of parts that animations move (sticks, levers, needles); --need-aniso N (4,
//                     the Quest cockpit sampler's ACZ_VR_RQ_COCKPIT_ANISO)
//     --need-keep RX  textures of meshes whose names match RX stay as built (default "mirror|retro|rearview": the mirror
//                     check reads their texels); the gloves (flag 16), the screens (flags 8-15) and glass (flag 1) also
//                     (the Quest's lean wet canopy reads its glass's tint at the ABSOLUTE level 3, rq_cockpit_lwet*.vert:
//                     a glass texture stored from a lower level would tint differently; every other cockpit read is
//                     relative: the material's texture(), the night markings' bias 3)
//     --cap PX        then at most PX x PX texels (a BC chain must fit the runtime's 32 MB staging buffer: 4096^2 BC7 = 22 MB) and
//                     at most 16384 on a side (the Adreno 740's maxImageDimension2D); the levels the cap takes beyond the need
//                     are reported. By area, not by the longer side: the F-2A's 100 x 10346 and the F/A-18E's 31 x 12228 are
//                     instrument tapes (the PFD's altitude numbers), 1 MB in BC7, magnified from the eye: a side cap at 4096
//                     would have taken two levels and left their digits 25 texels wide
//     --bc1-auto DB,P8[,P4]  an opaque texture goes to BC1 (4 bits a texel) when its BC1 top level holds PSNR RGB >= DB, at
//                     most P8 % of its texels with a channel off by more than 8 of 255 and at most P4 % by more than 4, else
//                     BC7 (8 bits): text, dials, glyphs and smooth glows fail BC1's 5:6:5 end points and stay BC7
//     --need-only     no output file: the need analysis, the sizes it gives, the report
//
// The clean-up changes no drawn pixel: textures identical in size and content become one (dedupe by a hash, then
// memcmp), textures no mesh with triangles uses are dropped, a texture of one solid colour becomes 4 x 4. Mesh records
// keep their index and order (the crack glass, the mirrors and the hand slots refer to meshes by index); only their
// texture index is remapped. The read-back check: every mesh's soup bit-identical to the input's (without --q16), the
// animations byte-identical, every texture's level 0 identical or (BC) measured.
//
// The BC7 encoder is Richard Geldreich's bc7enc (bc7enc_rdo, MIT or public domain; built from source,
// tools/cockpit/build_cpit_pack.sh): modes 1, 5, 6 and 7, perceptual weights. A block with both sides of the alpha cut
// (alpha < 128) is encoded again with more weight on alpha until no texel crosses the cut, or the last try's flips stay.
//
// Only reads IN; no game or network access. Build: tools/cockpit/build_cpit_pack.sh
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "bc7enc.h"
#include "bc7decomp.h"
#include "rgbcx.h"
#include "ps2_cpit4.h"

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;

struct Tex { u32 w, h; const u8 *px; };                                   // the input's records
struct OutTex { u32 w, h, src, flags, fmt; std::vector<u8> enc; const u8 *px; std::vector<u8> solid;
                double psnr_rgb, psnr_a; int maxerr; u64 flips; u64 cuts;
                // (--need / --cap / --bc1-auto) the source's size and texels, the levels left out, the need's least lambda and
                // the mesh that sets it, kept as built, the stored top level, the share over 8 of 255, BC1's try
                u32 sw, sh, skip, capskip; const u8 *spx; double need; int need_mesh; u8 full; std::vector<u8> top;
                double p8, p4, bc1_psnr, bc1_p8, bc1_p4; u64 need_tris, need_point; std::vector<float> pts; int pt_err; u32 pt_back; };
struct Mesh { const u8 *hdr; u32 tex, ntri; const u8 *v; };

static std::vector<u8> g_in;
static int g_uber = 1, g_threads = 8;
static double g_raw_below = 0.0, g_rt_db = 0.0, g_rt_p8 = 100.0;
static u64 g_rt_max = 0;
static u64 g_raw_max = ~(u64)0;
static double g_need_px = 0.0, g_need_margin = 1.5, g_need_lean = 0.15, g_need_near = 0.20, g_need_move = 0.10, g_need_aniso = 4.0;
static u32 g_cap = 0;
static double g_point_tol = 2.0;
static double g_bc1_db = 0.0, g_bc1_p8 = 0.0, g_bc1_p4 = 100.0;

// the distance from the origin (the design eye) to triangle abc (Ericson, Real-Time Collision Detection 5.1.5)
static double tri_dist0(const double *a, const double *b, const double *c) {
    double ab[3], ac[3], q[3];
    for (int k = 0; k < 3; k++) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; }
    auto dot = [](const double *x, const double *y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
    auto len = [](const double *x) { return sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); };
    double ap[3] = { -a[0], -a[1], -a[2] }, bp[3] = { -b[0], -b[1], -b[2] }, cp[3] = { -c[0], -c[1], -c[2] };
    double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return len(a);
    double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return len(b);
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k]; return len(q); }
    double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return len(c);
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); for (int k = 0; k < 3; k++) q[k] = a[k] + w * ac[k]; return len(q); }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int k = 0; k < 3; k++) q[k] = b[k] + w * (c[k] - b[k]);
        return len(q);
    }
    double den = 1.0 / (va + vb + vc), v = vb * den, w = vc * den;
    for (int k = 0; k < 3; k++) q[k] = a[k] + ab[k] * v + ac[k] * w;
    return len(q);
}
// (--need) the least lambda one triangle can be sampled at from the eye box: see the header. W, H the texture's texels.
// Returns -1e9 for texture coordinates collapsed to a point (magnified at any distance), 1e9 for a triangle of no area.
static double tri_need(const float *v0, const float *v1, const float *v2, double W, double H, double extra) {
    double p0[3] = { v0[0], v0[1], v0[2] }, p1[3] = { v1[0], v1[1], v1[2] }, p2[3] = { v2[0], v2[1], v2[2] };
    double e1[3], e2[3];
    for (int k = 0; k < 3; k++) { e1[k] = p1[k] - p0[k]; e2[k] = p2[k] - p0[k]; }
    double l1 = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    double cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
    double cl = sqrt(cx * cx + cy * cy + cz * cz);
    if (l1 < 1e-9 || cl < 1e-12) return 1e9;                                  // no area: no pixels
    double E00 = l1, E01 = (e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2]) / l1, E11 = cl / l1;   // e1 along a, e2 = (E01, E11)
    double F00 = ((double)v1[6] - v0[6]) * W, F01 = ((double)v2[6] - v0[6]) * W;
    double F10 = ((double)v1[7] - v0[7]) * H, F11 = ((double)v2[7] - v0[7]) * H;
    double J00 = F00 / E00, J01 = F01 / E11 - F00 * E01 / (E00 * E11);
    double J10 = F10 / E00, J11 = F11 / E11 - F10 * E01 / (E00 * E11);
    double T = J00 * J00 + J01 * J01 + J10 * J10 + J11 * J11, D = fabs(J00 * J11 - J01 * J10);
    double s1 = sqrt(0.5 * (T + sqrt(std::max(0.0, T * T - 4.0 * D * D)))), s2 = s1 > 0 ? D / s1 : 0.0;
    double r = std::max(s2, s1 / (sqrt(2.0) * g_need_aniso));                // texels per metre the sampler's lambda follows
    if (!(r > 0)) return -1e9;
    double d = std::max(g_need_near, tri_dist0(p0, p1, p2) - g_need_lean - extra);
    return log2(r * d / (g_need_px * g_need_margin));
}

static std::vector<u8> read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
#else
    fseek(f, 0, SEEK_END); long long n = ftell(f); fseek(f, 0, SEEK_SET);
#endif
    std::vector<u8> d((size_t)n);
    if (fread(d.data(), 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(1); }
    fclose(f);
    return d;
}
static u64 hash_bytes(const u8 *p, size_t n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { u64 v; memcpy(&v, p + i, 8); h = (h ^ v) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
template <class F> static void parallel_rows(u32 n, F fn) {
    std::atomic<u32> next(0);
    std::vector<std::thread> th;
    auto work = [&]() { for (u32 i; (i = next.fetch_add(1)) < n; ) fn(i); };
    int nt = std::max(1, g_threads);
    for (int t = 1; t < nt; t++) th.emplace_back(work);
    work();
    for (auto &x : th) x.join();
}

// one level to blocks, with its statistics
struct LevelStat { std::atomic<u64> flips{0}, cuts{0}; std::atomic<u64> se_rgb{0}, se_a{0}, over8{0}, over4{0}; std::atomic<int> maxerr{0}; };
static void encode_level(u32 fmt, const u8 *px, u32 w, u32 h, u8 *out, bool has_cut, LevelStat *st) {
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == CPIT_TF_BC1 ? 8u : 16u;
    bc7enc_compress_block_params base;
    bc7enc_compress_block_params_init(&base);
    base.m_uber_level = (u32)g_uber;
    parallel_rows(bh, [&](u32 by) {
        u8 in[64], dec[64];
        u64 se_rgb = 0, se_a = 0, flips = 0, cuts = 0, o8 = 0, o4 = 0; int mx = 0;
        for (u32 bx = 0; bx < bw; bx++) {
            for (u32 y = 0; y < 4; y++) for (u32 x = 0; x < 4; x++) {
                u32 sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                memcpy(in + (y * 4 + x) * 4, px + ((size_t)sy * w + sx) * 4, 4);
            }
            u8 *blk = out + ((size_t)by * bw + bx) * bs;
            if (fmt == CPIT_TF_BC1) { rgbcx::encode_bc1(18, blk, in, true, false); cpit_bc1_block(blk, dec); }
            else {
                bc7enc_compress_block_params p = base;
                bool lo = false, hi = false;
                for (int i = 0; i < 16; i++) (in[i * 4 + 3] < 128 ? lo : hi) = true;
                bool mixed = lo && hi;
                static const u32 aw[4] = { 32, 256, 2048, 16384 };
                u8 best[16] = { 0 }; int best_f = 1 << 30;
                for (int tr = 0; tr < (mixed ? 4 : 1); tr++) {
                    if (has_cut) p.m_weights[3] = aw[mixed ? tr : 0];
                    bc7enc_compress_block(blk, in, &p);
                    cpit_bc7_block(blk, dec);
                    int f = 0;
                    for (int i = 0; i < 16; i++) f += (in[i * 4 + 3] < 128) != (dec[i * 4 + 3] < 128);
                    if (f < best_f) { best_f = f; memcpy(best, blk, 16); }
                    if (!f) break;
                }
                memcpy(blk, best, 16);
                cpit_bc7_block(blk, dec);
            }
            for (u32 y = 0; y < 4 && by * 4 + y < h; y++) for (u32 x = 0; x < 4 && bx * 4 + x < w; x++) {
                const u8 *a = in + (y * 4 + x) * 4, *b = dec + (y * 4 + x) * 4;
                int m8 = 0;
                for (int c = 0; c < 3; c++) { int e = (int)a[c] - b[c]; se_rgb += (u64)(e * e); if (std::abs(e) > mx) mx = std::abs(e); m8 = std::max(m8, std::abs(e)); }
                int e = (int)a[3] - b[3]; se_a += (u64)(e * e); if (std::abs(e) > mx) mx = std::abs(e);
                if (fmt == CPIT_TF_BC7) m8 = std::max(m8, std::abs(e));         // (BC1 here is opaque: its alpha is not read)
                if (m8 > 8) o8++;
                if (m8 > 4) o4++;
                if (a[3] < 128) cuts++;
                if ((a[3] < 128) != (b[3] < 128)) flips++;
            }
        }
        st->se_rgb += se_rgb; st->se_a += se_a; st->flips += flips; st->cuts += cuts; st->over8 += o8; st->over4 += o4;
        int cur = st->maxerr.load();
        while (mx > cur && !st->maxerr.compare_exchange_weak(cur, mx)) {}
    });
}
static void box_down(const std::vector<u8> &s, u32 pw, u32 ph, std::vector<u8> &d, u32 &nw, u32 &nh) {
    nw = pw > 1 ? pw / 2 : 1; nh = ph > 1 ? ph / 2 : 1;                       // as cpit_view.c build_mips
    d.resize((size_t)nw * nh * 4);
    for (u32 y = 0; y < nh; y++) for (u32 x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
        u32 x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1), y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
        d[((size_t)y * nw + x) * 4 + c] = (u8)((s[((size_t)y0 * pw + x0) * 4 + c] + s[((size_t)y0 * pw + x1) * 4 + c]
                                               + s[((size_t)y1 * pw + x0) * 4 + c] + s[((size_t)y1 * pw + x1) * 4 + c] + 2) / 4);
    }
}
static void encode_tex(OutTex &t, u32 fmt) {
    t.fmt = fmt;
    size_t total = cpit4_tex_bytes(fmt, t.w, t.h);
    t.enc.resize(total);
    u32 lw = t.w, lh = t.h, nl = cpit4_levels(t.w, t.h);
    std::vector<u8> cur(t.px, t.px + (size_t)t.w * t.h * 4), nxt;
    size_t off = 0;
    bool has_cut = (t.flags & CPIT_TFL_CUT) != 0;
    for (u32 l = 0; l < nl; l++) {
        LevelStat st;
        encode_level(fmt, cur.data(), lw, lh, t.enc.data() + off, has_cut, &st);
        if (l == 0) {
            double n = (double)lw * lh;
            double mse_rgb = (double)st.se_rgb.load() / (3.0 * n), mse_a = (double)st.se_a.load() / n;
            t.psnr_rgb = mse_rgb > 0 ? 10.0 * log10(255.0 * 255.0 / mse_rgb) : 99.0;
            t.psnr_a = mse_a > 0 ? 10.0 * log10(255.0 * 255.0 / mse_a) : 99.0;
            t.maxerr = st.maxerr.load(); t.flips = st.flips.load(); t.cuts = st.cuts.load();
            t.p8 = 100.0 * (double)st.over8.load() / n; t.p4 = 100.0 * (double)st.over4.load() / n;
        }
        off += cpit4_level_bytes(fmt, lw, lh);
        if (l + 1 < nl) { u32 nw, nh; box_down(cur, lw, lh, nxt, nw, nh); cur.swap(nxt); lw = nw; lh = nh; }
    }
}
static void out_write(FILE *f, const void *p, size_t n) { if (fwrite(p, 1, n, f) != n) { perror("write"); exit(1); } }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: cpit_pack IN.cpit OUT.cpit [--format 3|4] [--tex bc7|raw] [--q16] [--no-clean] [--uber N] [--threads N] [--report F.tsv] [--bc1 REGEX]\n"); return 2; }
    const char *inp = argv[1], *outp = argv[2], *report = nullptr, *bc1_rx = nullptr;
    const char *keep_rx = "mirror|retro|rearview";
    int format = 4, q16 = 0, clean = 1, verify = 1, need_only = 0; std::string texm = "bc7";
    if (const char *e = getenv("NUMBER_OF_PROCESSORS")) g_threads = std::min(atoi(e), 24);
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--format") format = atoi(argv[++i]);
        else if (a == "--tex") texm = argv[++i];
        else if (a == "--q16") q16 = 1;
        else if (a == "--no-clean") clean = 0;
        else if (a == "--no-verify") verify = 0;
        else if (a == "--uber") g_uber = atoi(argv[++i]);
        else if (a == "--threads") g_threads = atoi(argv[++i]);
        else if (a == "--report") report = argv[++i];
        else if (a == "--raw-below") g_raw_below = atof(argv[++i]);
        else if (a == "--raw-max-bytes") g_raw_max = strtoull(argv[++i], nullptr, 10);
        else if (a == "--raw-text") { unsigned long long mb = 0; if (sscanf(argv[++i], "%lf,%lf,%llu", &g_rt_db, &g_rt_p8, &mb) != 3) { fprintf(stderr, "--raw-text DB,P8,MAXBYTES\n"); return 2; } g_rt_max = mb; }
        else if (a == "--bc1") bc1_rx = argv[++i];
        else if (a == "--need") g_need_px = atof(argv[++i]);
        else if (a == "--need-margin") g_need_margin = atof(argv[++i]);
        else if (a == "--need-lean") g_need_lean = atof(argv[++i]);
        else if (a == "--need-near") g_need_near = atof(argv[++i]);
        else if (a == "--need-move") g_need_move = atof(argv[++i]);
        else if (a == "--need-aniso") g_need_aniso = std::max(1.0, atof(argv[++i]));
        else if (a == "--need-keep") keep_rx = argv[++i];
        else if (a == "--need-point-tol") g_point_tol = atof(argv[++i]);
        else if (a == "--need-only") { need_only = 1; verify = 0; }
        else if (a == "--cap") g_cap = (u32)atoi(argv[++i]);
        else if (a == "--bc1-auto") { if (sscanf(argv[++i], "%lf,%lf,%lf", &g_bc1_db, &g_bc1_p8, &g_bc1_p4) < 2) { fprintf(stderr, "--bc1-auto DB,P8[,P4]\n"); return 2; } }
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (g_need_px > 0 && g_need_margin <= 0) { fprintf(stderr, "--need-margin must be > 0\n"); return 2; }
    g_in = read_all(inp);
    const u8 *d = g_in.data(); size_t n = g_in.size();
    if (n < 20 || memcmp(d, "ACZCPIT3", 8)) { fprintf(stderr, "%s: not ACZCPIT3\n", inp); return 1; }
    u32 ntex, nmesh, nanim;
    memcpy(&ntex, d + 8, 4); memcpy(&nmesh, d + 12, 4); memcpy(&nanim, d + 16, 4);
    size_t p = 20;
    std::vector<Tex> tex(ntex);
    for (u32 i = 0; i < ntex; i++) {
        u32 w, h; memcpy(&w, d + p, 4); memcpy(&h, d + p + 4, 4);
        tex[i].w = w; tex[i].h = h; tex[i].px = d + p + 8;
        p += 8 + (size_t)w * h * 4;
    }
    size_t anim0 = p;
    for (u32 i = 0; i < nanim; i++) { u32 nt; memcpy(&nt, d + p + 36, 4); p += 40 + (size_t)nt * 8; }
    size_t anim1 = p;
    std::vector<Mesh> mesh(nmesh);
    std::vector<u8> used(ntex, 0);
    for (u32 i = 0; i < nmesh; i++) {
        Mesh &m = mesh[i];
        m.hdr = d + p; memcpy(&m.tex, d + p + 32, 4); memcpy(&m.ntri, d + p + 80, 4);
        m.v = d + p + 84;
        if (m.tex < ntex && m.ntri) used[m.tex] = 1;
        p += 84 + (size_t)m.ntri * 108;
    }
    if (p != n) { fprintf(stderr, "%s: %zu bytes after the meshes (%zu)\n", inp, n - p, n); return 1; }

    // ---- the clean-up: the map old texture -> new texture
    std::vector<u32> remap(ntex, 0xFFFFFFFFu);
    std::vector<OutTex> out;
    std::unordered_map<u64, std::vector<u32>> seen;
    u64 dup_b = 0, unref_b = 0, solid_b = 0; u32 dup_n = 0, unref_n = 0, solid_n = 0;
    for (u32 i = 0; i < ntex; i++) {
        size_t bytes = (size_t)tex[i].w * tex[i].h * 4;
        if (clean && !used[i]) { unref_b += bytes; unref_n++; continue; }
        u64 h = hash_bytes(tex[i].px, bytes) ^ ((u64)tex[i].w << 32 | tex[i].h);
        if (clean) {
            bool found = false;
            for (u32 o : seen[h]) {
                const OutTex &t = out[o];
                if (tex[t.src].w == tex[i].w && tex[t.src].h == tex[i].h && !memcmp(tex[t.src].px, tex[i].px, bytes)) { remap[i] = o; found = true; break; }
            }
            if (found) { dup_b += bytes; dup_n++; continue; }
        }
        OutTex t{}; t.w = tex[i].w; t.h = tex[i].h; t.src = i; t.px = tex[i].px; t.fmt = CPIT_TF_RGBA8;
        t.sw = t.w; t.sh = t.h; t.spx = t.px; t.need = 1e9; t.need_mesh = -1; t.bc1_psnr = t.bc1_p8 = t.bc1_p4 = -1;
        bool cut = false, op = true, solid = true;
        for (size_t k = 0; k < bytes; k += 4) {
            const u8 *q = t.px + k;
            if (q[3] < 128) cut = true;
            if (q[3] != 255) op = false;
            if (memcmp(q, t.px, 4)) solid = false;
        }
        t.flags = (cut ? CPIT_TFL_CUT : 0u) | (op ? CPIT_TFL_OPAQUE : 0u);
        if (clean && solid && (t.w > 4 || t.h > 4)) {                 // one colour: 4 x 4 samples the same at any UV
            solid_b += bytes - 64; solid_n++;
            t.solid.resize(64);
            for (int k = 0; k < 64; k += 4) memcpy(&t.solid[k], t.px, 4);
            t.w = t.h = 4; t.px = t.solid.data();
        }
        remap[i] = (u32)out.size();
        seen[h].push_back((u32)out.size());
        out.emplace_back(std::move(t));
        if (!out.back().solid.empty()) out.back().px = out.back().solid.data();
    }
    u64 old_tex_b = anim0 - 20 - 8 * (u64)ntex, new_rgba_b = 0;
    double vram_old = 0, vram_new = 0;
    for (u32 i = 0; i < ntex; i++) {
        u32 nl = cpit4_levels(tex[i].w, tex[i].h);
        for (u32 l = 0; l < nl; l++) vram_old += (double)cpit4_lv_dim(tex[i].w, l) * cpit4_lv_dim(tex[i].h, l) * 4.0;
    }

    // ---- (--need / --cap) the levels each texture leaves out: see the header
    double need_mb[3] = { 0, 0, 0 };                                  // RGBA8 + mips: as built, after the need, after the cap
    if (g_need_px > 0 || g_cap) {
        std::vector<u32> aprop(nanim, 0);
        {   size_t q = anim0;
            for (u32 i = 0; i < nanim; i++) { u32 nt; memcpy(&aprop[i], d + q, 4); memcpy(&nt, d + q + 36, 4); q += 40 + (size_t)nt * 8; } }
        std::regex krx(keep_rx, std::regex::icase);
        for (u32 i = 0; i < nmesh; i++) {
            const Mesh &m = mesh[i];
            if (m.tex >= ntex || !m.ntri || remap[m.tex] == 0xFFFFFFFFu) continue;
            OutTex &t = out[remap[m.tex]];
            u32 fl, an[4]; memcpy(&fl, m.hdr + 52, 4); memcpy(an, m.hdr + 56, 16);
            char nm[33]; memcpy(nm, m.hdr, 32); nm[32] = 0;
            if ((fl & (1u | 16u)) || ((fl >> 8) & 255u) || (keep_rx[0] && std::regex_search(nm, krx))) {
                if (!t.full || t.need_mesh < 0) t.need_mesh = (int)i;
                t.full = 1; t.need = -1e9; continue;
            }
            if (g_need_px <= 0 || !t.solid.empty()) continue;
            bool moving = false;
            for (int k = 0; k < 4; k++)
                if (an[k] < nanim && !(aprop[an[k]] & (0x100u | 0x200u | 0x1000u))) moving = true;   // rotate / translate
            double W = tex[m.tex].w, H = tex[m.tex].h;
            for (u32 k = 0; k < m.ntri; k++) {
                float vv[3][9];
                memcpy(vv, m.v + (size_t)k * 108, 108);
                double l = tri_need(vv[0], vv[1], vv[2], W, H, moving ? g_need_move : 0.0);
                if (l >= 1e8) continue;
                t.need_tris++;
                if (l <= -1e8) {                                      // a point: checked by its colour below
                    t.need_point++;
                    if (t.pts.size() < 2u * 8192u) { t.pts.push_back(vv[0][6]); t.pts.push_back(vv[0][7]); }
                    continue;
                }
                if (l < t.need) { t.need = l; t.need_mesh = (int)i; }
            }
        }
        for (OutTex &t : out) {
            u32 nl = cpit4_levels(t.w, t.h), s = 0;
            for (u32 l = 0; l < nl; l++) need_mb[0] += (double)cpit4_lv_dim(t.w, l) * cpit4_lv_dim(t.h, l) * 4.0;
            if (g_need_px > 0 && !t.full && t.solid.empty() && t.need >= 1.0 && t.need < 1e8) s = (u32)floor(t.need);
            if (g_need_px > 0 && !t.full && t.solid.empty() && t.need >= 1e8 && !t.pts.empty()) s = nl - 1;   // (points only)
            if (s > nl - 1) s = nl - 1;                               // (a texture no triangle with area samples: as built)
            // the box chain's levels up to s (level l at chain[l]); the points' colours: level 0's bilinear texel at their texture
            // coordinates (magnified: the sampler reads level 0) against level s's, at most --need-point-tol apart, else one
            // level less is left out
            std::vector<std::vector<u8>> chain;
            std::vector<u32> cw, ch;
            auto build = [&](u32 upto) {
                if (chain.empty()) { chain.emplace_back(t.px, t.px + (size_t)t.w * t.h * 4); cw.push_back(t.w); ch.push_back(t.h); }
                while (chain.size() <= upto) {
                    std::vector<u8> nx; u32 nw, nh;
                    box_down(chain.back(), cw.back(), ch.back(), nx, nw, nh);
                    chain.push_back(std::move(nx)); cw.push_back(nw); ch.push_back(nh);
                }
            };
            auto bil = [](const std::vector<u8> &im, u32 w, u32 h, double u, double v, double *o) {
                double x = (u - floor(u)) * w - 0.5, y = (v - floor(v)) * h - 0.5;
                long x0 = (long)floor(x), y0 = (long)floor(y); double fx = x - x0, fy = y - y0;
                long xa = ((x0 % (long)w) + w) % w, xb = (xa + 1) % w, ya = ((y0 % (long)h) + h) % h, yb = (ya + 1) % h;
                for (int k = 0; k < 4; k++)
                    o[k] = (im[((size_t)ya * w + xa) * 4 + k] * (1 - fx) + im[((size_t)ya * w + xb) * 4 + k] * fx) * (1 - fy)
                         + (im[((size_t)yb * w + xa) * 4 + k] * (1 - fx) + im[((size_t)yb * w + xb) * 4 + k] * fx) * fy;
            };
            t.pt_err = 0;
            u32 s0 = s;
            while (s > 0 && !t.pts.empty()) {
                build(s);
                double worst = 0;
                for (size_t k = 0; k + 1 < t.pts.size() && worst <= g_point_tol; k += 2) {
                    double a[4], b[4];
                    bil(chain[0], cw[0], ch[0], t.pts[k], t.pts[k + 1], a);
                    bil(chain[s], cw[s], ch[s], t.pts[k], t.pts[k + 1], b);
                    for (int q = 0; q < 4; q++) worst = std::max(worst, fabs(a[q] - b[q]));
                }
                t.pt_err = (int)ceil(worst);
                if (worst <= g_point_tol) break;
                s--;
            }
            t.pt_back = s0 - s;
            for (u32 l = s; l < nl; l++) need_mb[1] += (double)cpit4_lv_dim(t.w, l) * cpit4_lv_dim(t.h, l) * 4.0;
            u32 c = s;
            while (g_cap && c + 1 < nl && ((u64)cpit4_lv_dim(t.w, c) * cpit4_lv_dim(t.h, c) > (u64)g_cap * g_cap
                                           || std::max(cpit4_lv_dim(t.w, c), cpit4_lv_dim(t.h, c)) > 16384u)) c++;
            for (u32 l = c; l < nl; l++) need_mb[2] += (double)cpit4_lv_dim(t.w, l) * cpit4_lv_dim(t.h, l) * 4.0;
            t.skip = c; t.capskip = c - s;
            if (c) {                                                  // the stored top: the 2 x 2 box chain's level c
                build(c);
                t.top.swap(chain[c]); t.px = t.top.data(); t.w = cw[c]; t.h = ch[c];
            }
        }
    }

    // ---- format 4's encoder
    rgbcx::init();
    bc7enc_compress_block_init();
    std::vector<u8> bc1_ok(out.size(), 0);
    if (bc1_rx) {
        std::regex rx(bc1_rx, std::regex::icase);
        std::vector<u8> any(out.size(), 0), all(out.size(), 1);
        for (u32 i = 0; i < nmesh; i++) {
            const Mesh &m = mesh[i];
            if (m.tex >= ntex || !m.ntri || remap[m.tex] == 0xFFFFFFFFu) continue;
            char nm[33]; memcpy(nm, m.hdr, 32); nm[32] = 0;
            any[remap[m.tex]] = 1;
            if (!std::regex_search(nm, rx)) all[remap[m.tex]] = 0;
        }
        for (size_t i = 0; i < out.size(); i++) bc1_ok[i] = any[i] && all[i] && (out[i].flags & CPIT_TFL_OPAQUE);
    }
    auto need_summary = [&]() {
        if (!(g_need_px > 0 || g_cap)) return;
        u32 ns = 0, nc = 0, nf = 0, np = 0;
        u32 npb = 0;
        for (const OutTex &t : out) { ns += t.skip > t.capskip; nc += t.capskip > 0; nf += t.full; np += t.need_point > 0 && !t.full; npb += t.pt_back > 0; }
        printf("  need (--need %.0f px/tan x %.2f, lean %.2f, near %.2f, move %.2f, aniso %.0f; --cap %u): RGBA8+mips %.1f -> %.1f MB by need -> %.1f MB "
               "with the cap; %u textures left levels out by need, %u more by the cap, %u kept as built (glass, gloves, screens, mirrors), "
               "%u with triangles whose texture coordinates collapse to a point (%u of them kept levels for those points' colours)\n", g_need_px, g_need_margin, g_need_lean, g_need_near,
               g_need_move, g_need_aniso, g_cap, need_mb[0] / 1e6, need_mb[1] / 1e6, need_mb[2] / 1e6, ns, nc, nf, np, npb);
    };
    auto char_name = [&](int mi) { std::string s; if (mi >= 0) { char nm[33]; memcpy(nm, mesh[mi].hdr, 32); nm[32] = 0; s = nm; } else s = "-"; return s; };
    if (need_only) {
        need_summary();
        if (report) {
            FILE *r = fopen(report, "w");
            if (r) {
                fprintf(r, "new\tsrc\tsw\tsh\tneed\tskip\tcapskip\tw\th\tfull\tflags\tneed_tris\tpoint_tris\tneed_mesh\n");
                for (size_t i = 0; i < out.size(); i++) {
                    const OutTex &t = out[i];
                    fprintf(r, "%zu\t%u\t%u\t%u\t%.2f\t%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%llu\t%s\n", i, t.src, t.sw, t.sh, t.need > 1e8 ? 99.0 : t.need < -1e8 ? -99.0 : t.need,
                            t.skip, t.capskip, t.w, t.h, t.full, t.flags, (unsigned long long)t.need_tris, (unsigned long long)t.need_point, char_name(t.need_mesh).c_str());
                }
                fclose(r);
            }
        }
        return 0;
    }
    u64 enc_b = 0, raw_b = 0; u32 raw_n = 0, bc1_n = 0, rawt_n = 0;
    for (size_t i = 0; i < out.size(); i++) {
        OutTex &t = out[i];
        new_rgba_b += (u64)t.w * t.h * 4;
        t.psnr_rgb = t.psnr_a = 99.0;
        if (format == 4 && texm == "bc7" && (t.w > 8 || t.h > 8)) {   // (at most 8 x 8: RGBA8 is as small, and exact: the solids)
            u32 f0 = bc1_ok[i] ? CPIT_TF_BC1 : CPIT_TF_BC7;
            if (g_bc1_db > 0 && !bc1_ok[i] && !t.full && (t.flags & CPIT_TFL_OPAQUE)) {   // --bc1-auto: BC1 where it holds
                encode_tex(t, CPIT_TF_BC1);
                t.bc1_psnr = t.psnr_rgb; t.bc1_p8 = t.p8; t.bc1_p4 = t.p4;
                f0 = t.psnr_rgb >= g_bc1_db && t.p8 <= g_bc1_p8 && t.p4 <= g_bc1_p4 ? CPIT_TF_BC1 : CPIT_TF_BC7;
            }
            if (t.enc.empty() || t.fmt != f0) encode_tex(t, f0);
            bool rt = g_rt_max && (u64)t.w * t.h * 4 <= g_rt_max && (t.psnr_rgb < g_rt_db || t.p8 > g_rt_p8);   // --raw-text
            if (rt) rawt_n++;
            if (rt || (t.psnr_rgb < g_raw_below && (u64)t.w * t.h * 4 <= g_raw_max)) { t.fmt = CPIT_TF_RGBA8; t.enc.clear(); t.enc.shrink_to_fit(); raw_n++; raw_b += (u64)t.w * t.h * 4; }
            if (t.fmt == CPIT_TF_BC1) bc1_n++;
        }
        if (t.fmt == CPIT_TF_RGBA8) {
            u32 nl = cpit4_levels(t.w, t.h);
            for (u32 l = 0; l < nl; l++) vram_new += (double)cpit4_lv_dim(t.w, l) * cpit4_lv_dim(t.h, l) * 4.0;
            enc_b += (u64)t.w * t.h * 4;
        } else { vram_new += (double)t.enc.size(); enc_b += t.enc.size(); }
    }

    // ---- write
    std::string tmp = std::string(outp) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { perror(outp); return 1; }
    u32 hdr[3] = { (u32)out.size(), nmesh, nanim };
    out_write(f, format == 4 ? "ACZCPIT4" : "ACZCPIT3", 8);
    out_write(f, hdr, 12);
    for (const OutTex &t : out) {
        if (format == 4) {
            u32 rec[4] = { t.w, t.h | CPIT4_TAG, t.fmt, t.flags };
            out_write(f, rec, 16);
        } else {
            u32 rec[2] = { t.w, t.h };
            out_write(f, rec, 8);
        }
        if (t.fmt == CPIT_TF_RGBA8) out_write(f, t.px, (size_t)t.w * t.h * 4);
        else out_write(f, t.enc.data(), t.enc.size());
    }
    out_write(f, d + anim0, anim1 - anim0);
    u64 mesh_b = 0, nv_tot = 0, q16_n = 0, soup_b = 0, ixb = 0;
    for (u32 i = 0; i < nmesh; i++) {
        const Mesh &m = mesh[i];
        u8 h84[84]; memcpy(h84, m.hdr, 84);
        u32 nt = m.tex < ntex ? remap[m.tex] : 0xFFFFFFFFu;
        if (m.tex >= ntex) nt = m.tex;
        memcpy(h84 + 32, &nt, 4);
        out_write(f, h84, 84);
        mesh_b += 84; soup_b += 84 + (u64)m.ntri * 108;
        if (!m.ntri) continue;
        if (format != 4) { out_write(f, m.v, (size_t)m.ntri * 108); mesh_b += (u64)m.ntri * 108; continue; }
        // the first use of each vertex, as the loader's own dedupe (ps2_video.c: first-use order)
        struct KeyHash { size_t operator()(u64 x) const { return (size_t)x; } };
        std::unordered_map<u64, std::vector<u32>, KeyHash> vmap;
        vmap.reserve((size_t)m.ntri * 2);
        std::vector<u32> ix((size_t)m.ntri * 3);
        std::vector<u8> uq;
        uq.reserve((size_t)m.ntri * 36);
        u32 nv = 0;
        for (size_t k = 0; k < (size_t)m.ntri * 3; k++) {
            const u8 *v = m.v + k * 36;
            u64 hh = hash_bytes(v, 36);
            auto &bucket = vmap[hh];
            u32 got = 0xFFFFFFFFu;
            for (u32 c : bucket) if (!memcmp(&uq[(size_t)c * 36], v, 36)) { got = c; break; }
            if (got == 0xFFFFFFFFu) { got = nv++; bucket.push_back(got); uq.insert(uq.end(), v, v + 36); }
            ix[k] = got;
        }
        nv_tot += nv;
        u32 vfmt = CPIT4_V_F32;
        float qh[8]; std::vector<u8> qv;
        if (q16) {                                                    // --q16: only where the loss stays small
            const float pstep = 1.0f / 20000.0f;                      // 0.05 mm: 0.14 px at 0.5 m, 25 px / degree
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f }, ulo[2] = { 1e30f, 1e30f }, uhi[2] = { -1e30f, -1e30f };
            bool ok = true;
            for (u32 k = 0; k < nv && ok; k++) {
                float a[9]; memcpy(a, &uq[(size_t)k * 36], 36);
                for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], a[j]); hi[j] = std::max(hi[j], a[j]); }
                for (int j = 0; j < 2; j++) { ulo[j] = std::min(ulo[j], a[6 + j]); uhi[j] = std::max(uhi[j], a[6 + j]); }
                float nl = a[3] * a[3] + a[4] * a[4] + a[5] * a[5];
                if (nl < 0.98f || nl > 1.02f || a[8] < 0.0f || a[8] > 1.0f) ok = false;
            }
            u32 tw = 1, th = 1;
            if (m.tex < ntex) { tw = tex[m.tex].w; th = tex[m.tex].h; }
            for (int j = 0; j < 3 && ok; j++) { if ((hi[j] - lo[j]) / pstep > 65534.0f) ok = false; qh[j] = floorf(lo[j] / pstep) * pstep; }
            qh[3] = pstep;
            qh[4] = ulo[0]; qh[5] = ulo[1];
            qh[6] = uhi[0] > ulo[0] ? (uhi[0] - ulo[0]) / 65535.0f : 1.0f; qh[7] = uhi[1] > ulo[1] ? (uhi[1] - ulo[1]) / 65535.0f : 1.0f;
            if (m.tex < ntex && (qh[6] * (float)tw > 0.0625f || qh[7] * (float)th > 0.0625f)) ok = false;
            if (ok) {
                qv.resize((size_t)nv * 16);
                for (u32 k = 0; k < nv; k++) {
                    float a[9]; memcpy(a, &uq[(size_t)k * 36], 36);
                    u8 *o = &qv[(size_t)k * 16];
                    uint16_t q[3], t2[2]; int16_t oc[2];
                    for (int j = 0; j < 3; j++) q[j] = (uint16_t)std::min(65535.0f, std::max(0.0f, floorf((a[j] - qh[j]) / pstep + 0.5f)));
                    float l1 = fabsf(a[3]) + fabsf(a[4]) + fabsf(a[5]);
                    float nx = a[3] / l1, ny = a[4] / l1;
                    if (a[5] < 0) { float ox = nx; nx = (1.0f - fabsf(ny)) * (ox >= 0 ? 1.0f : -1.0f); ny = (1.0f - fabsf(ox)) * (ny >= 0 ? 1.0f : -1.0f); }
                    oc[0] = (int16_t)floorf(nx * 32767.0f + 0.5f); oc[1] = (int16_t)floorf(ny * 32767.0f + 0.5f);
                    for (int j = 0; j < 2; j++) t2[j] = (uint16_t)std::min(65535.0f, std::max(0.0f, floorf((a[6 + j] - qh[4 + j]) / qh[6 + j] + 0.5f)));
                    memcpy(o, q, 6); memcpy(o + 6, oc, 4); memcpy(o + 10, t2, 4);
                    o[14] = (u8)floorf(a[8] * 255.0f + 0.5f); o[15] = 0;
                }
                vfmt = CPIT4_V_Q16; q16_n++;
            }
        }
        u32 vh[2] = { nv, vfmt };
        out_write(f, vh, 8); mesh_b += 8;
        if (vfmt == CPIT4_V_Q16) { out_write(f, qh, 32); out_write(f, qv.data(), qv.size()); mesh_b += 32 + qv.size(); }
        else { out_write(f, uq.data(), uq.size()); mesh_b += uq.size(); }
        size_t ib;
        if (nv <= 65536u) {
            std::vector<uint16_t> s(ix.size());
            for (size_t k = 0; k < ix.size(); k++) s[k] = (uint16_t)ix[k];
            ib = s.size() * 2; out_write(f, s.data(), ib);
        } else { ib = ix.size() * 4; out_write(f, ix.data(), ib); }
        static const u8 zero[4] = { 0, 0, 0, 0 };
        size_t pad = ((ib + 3) & ~(size_t)3) - ib;
        if (pad) out_write(f, zero, pad);
        mesh_b += ib + pad; ixb += ib + pad;
    }
    fclose(f);
    remove(outp);
    if (rename(tmp.c_str(), outp)) { perror("rename"); return 1; }

    // ---- read the output back and compare it with the input
    long long bad_mesh = -1;
    if (verify) {
        std::vector<u8> o = read_all(outp);
        const u8 *od = o.data(); size_t on = o.size();
        size_t q = 20; u32 ont; memcpy(&ont, od + 8, 4);
        if (ont != out.size()) { fprintf(stderr, "verify: %u textures\n", ont); return 1; }
        for (u32 i = 0; i < ont; i++) {
            u32 w, h, fm, fl; size_t tx, by;
            if (!cpit4_tex_rec(od, on, q, &w, &h, &fm, &fl, &tx, &by)) { fprintf(stderr, "verify: texture %u\n", i); return 1; }
            const OutTex &t = out[i];
            if (w != t.w || h != t.h || fm != t.fmt) { fprintf(stderr, "verify: texture %u header\n", i); return 1; }
            if (fm == CPIT_TF_RGBA8 && memcmp(od + tx, t.px, by)) { fprintf(stderr, "verify: texture %u texels\n", i); return 1; }
            if (fm != CPIT_TF_RGBA8) {                                // the same decoder as the runtime and bc7decomp agree
                if (memcmp(od + tx, t.enc.data(), by)) { fprintf(stderr, "verify: texture %u blocks\n", i); return 1; }
                if (fm == CPIT_TF_BC7) {
                    size_t nb = cpit4_level_bytes(fm, w, h) / 16;
                    for (size_t b = 0; b < nb; b += 1 + nb / 4096) {
                        u8 a[64]; bc7decomp::color_rgba r[16];
                        cpit_bc7_block(od + tx + b * 16, a);
                        bc7decomp::unpack_bc7(od + tx + b * 16, r);
                        if (memcmp(a, r, 64)) { fprintf(stderr, "verify: texture %u block %zu: decoders differ\n", i, b); return 1; }
                    }
                }
            }
            q = tx + by;
        }
        if (memcmp(od + q, d + anim0, anim1 - anim0)) { fprintf(stderr, "verify: animations differ\n"); return 1; }
        q += anim1 - anim0;
        bad_mesh = 0;
        std::vector<u8> soup;
        for (u32 i = 0; i < nmesh; i++) {
            const Mesh &m = mesh[i];
            if (memcmp(od + q, m.hdr, 32) || memcmp(od + q + 36, m.hdr + 36, 48)) { fprintf(stderr, "verify: mesh %u header\n", i); return 1; }
            u32 tt; memcpy(&tt, od + q + 32, 4);
            if (m.tex < ntex && m.ntri) {                             // the same pixels through the remapped index
                const OutTex &t = out[tt];
                if (t.solid.empty() && (t.sw != tex[m.tex].w || t.sh != tex[m.tex].h || memcmp(t.spx, tex[m.tex].px, (size_t)t.sw * t.sh * 4))) { fprintf(stderr, "verify: mesh %u texture\n", i); return 1; }
                if (!t.solid.empty() && memcmp(t.px, tex[m.tex].px, 4)) { fprintf(stderr, "verify: mesh %u solid\n", i); return 1; }
            }
            q += 84;
            if (!m.ntri) continue;
            if (format != 4) { if (memcmp(od + q, m.v, (size_t)m.ntri * 108)) bad_mesh++; q += (size_t)m.ntri * 108; continue; }
            soup.resize((size_t)m.ntri * 108);
            q += cpit4_mesh_expand(od + q, m.ntri, soup.data());
            if (!q16 && memcmp(soup.data(), m.v, soup.size())) bad_mesh++;
        }
        if (q != on) { fprintf(stderr, "verify: %zu bytes left at the end\n", on - q); return 1; }
        if (bad_mesh) { fprintf(stderr, "verify: %lld meshes differ\n", bad_mesh); return 1; }
    }

    // ---- report
    double min_psnr = 99, wse = 0, wn = 0; u64 flips = 0, cuts = 0; int mxe = 0;
    for (const OutTex &t : out) {
        if (t.fmt == CPIT_TF_RGBA8) continue;
        min_psnr = std::min(min_psnr, t.psnr_rgb);
        double npx = (double)t.w * t.h;
        wse += pow(10.0, -t.psnr_rgb / 10.0) * npx; wn += npx;
        flips += t.flips; cuts += t.cuts; mxe = std::max(mxe, t.maxerr);
    }
    if (report) {
        FILE *r = fopen(report, "w");
        if (r) {
            fprintf(r, "new\tsrc\tw\th\tfmt\tflags\tbytes\tpsnr_rgb\tpsnr_a\tmaxerr\tcut_texels\tcut_flips\tsw\tsh\tneed\tskip\tcapskip\tfull\tover8_pct\tbc1_psnr\tbc1_over8_pct\tneed_mesh\tover4_pct\tbc1_over4_pct\tpoint_back\n");
            for (size_t i = 0; i < out.size(); i++) {
                const OutTex &t = out[i];
                fprintf(r, "%zu\t%u\t%u\t%u\t%u\t%u\t%zu\t%.2f\t%.2f\t%d\t%llu\t%llu\t%u\t%u\t%.2f\t%u\t%u\t%u\t%.4f\t%.2f\t%.4f\t%s\t%.4f\t%.4f\t%u\n", i, t.src, t.w, t.h, t.fmt, t.flags,
                        t.fmt == CPIT_TF_RGBA8 ? (size_t)t.w * t.h * 4 : t.enc.size(), t.psnr_rgb, t.psnr_a, t.maxerr,
                        (unsigned long long)t.cuts, (unsigned long long)t.flips, t.sw, t.sh,
                        t.need > 1e8 ? 99.0 : t.need < -1e8 ? -99.0 : t.need, t.skip, t.capskip, t.full, t.p8, t.bc1_psnr, t.bc1_p8,
                        char_name(t.need_mesh).c_str(), t.p4, t.bc1_p4, t.pt_back);
            }
            fclose(r);
        }
    }
    u64 out_b = 0;
    { FILE *c = fopen(outp, "rb"); if (c) { _fseeki64(c, 0, SEEK_END); out_b = (u64)_ftelli64(c); fclose(c); } }
    printf("cpit_pack %s -> %s format %d\n", inp, outp, format);
    printf("  file %.1f -> %.1f MB; textures %u -> %zu, %.1f -> %.1f MB; meshes %.1f -> %.1f MB (%llu unique vertices of %llu, %llu at 16 bytes, indices %.1f MB)\n",
           n / 1e6, out_b / 1e6, ntex, out.size(), old_tex_b / 1e6, enc_b / 1e6, soup_b / 1e6, mesh_b / 1e6, (unsigned long long)nv_tot,
           (unsigned long long)((soup_b - 84ull * nmesh) / 36), (unsigned long long)q16_n, ixb / 1e6);
    printf("  clean-up: %u duplicates %.1f MB, %u unreferenced %.1f MB, %u solids %.1f MB (RGBA8 left %.1f MB)\n", dup_n, dup_b / 1e6, unref_n,
           unref_b / 1e6, solid_n, solid_b / 1e6, new_rgba_b / 1e6);
    printf("  texture VRAM %.1f -> %.1f MB\n", vram_old / 1e6, vram_new / 1e6);
    need_summary();
    if (g_bc1_db > 0) printf("  --bc1-auto %.1f dB, %.3f %% over 8, %.2f %% over 4: %u textures BC1\n", g_bc1_db, g_bc1_p8, g_bc1_p4, bc1_n);
    if (raw_n) printf("  kept RGBA8 (--raw-below %.1f dB; --raw-text %.1f dB / %.2f %% over 8 up to %llu bytes: %u of them): %u textures, %.1f MB\n", g_raw_below,
                      g_rt_db, g_rt_p8, (unsigned long long)g_rt_max, rawt_n, raw_n, raw_b / 1e6);
    if (wn > 0) printf("  BC: PSNR RGB %.2f dB overall (texel-weighted), %.2f dB the worst texture; max channel error %d; alpha-cut flips %llu of %llu cut texels\n",
                       wn > 0 && wse > 0 ? 10.0 * log10(1.0 / (wse / wn)) : 99.0, min_psnr, mxe, (unsigned long long)flips, (unsigned long long)cuts);
    if (verify) printf("  read-back: animations identical, %s\n", q16 ? "meshes expanded (--q16: not compared)" : "every mesh's soup bit-identical");
    return 0;
}
