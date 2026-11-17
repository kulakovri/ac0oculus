#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit's wet canopy, the PC's look (rq_cockpit.inc, ACZ_VR_RQ_COCKPIT_WET=lean2). The tiles hold the drops' parameters, not their
   anti-aliased picture (rqw_bake2: the lens through the rim, rho, the half width in metres, the tail parameter, the presence draw), and this
   stage applies cockpit.frag's own formulas with the pixel's true footprint on the glass: the rim's width, the fade of a drop too small to draw,
   how far its lens resolves (per drop). Veils are blend factors, so the eye-sized world stays behind them: only a resolved drop (the world bent
   through it, the feed), the film and the rime (the small mip chain of the feed, a blur) read the world. At most 6 reads, mediump. */
precision mediump float;
precision mediump int;
layout(location = 0) in vec4 v_gc;
layout(location = 1) in highp vec4 v_tc;
layout(location = 2) in highp vec2 v_nu;
layout(location = 3) in vec4 v_lens;
layout(location = 4) in vec4 v_amb;
layout(location = 5) in vec4 v_sp;
layout(location = 6) in vec4 v_fr;
layout(location = 7) in vec4 v_lh;
layout(location = 8) in highp vec4 v_x;          /* xy the second rivulet layer, zw the rime map */
layout(location = 9) in vec3 v_gl;               /* n.L, the ray in the water against the sun, -n.view axis */
layout(location = 10) in highp vec4 v_w;         /* xy the glass normal in the frame image (dir_uv), zw the view axis' component of U, V */
#ifdef VUV
layout(location = 12) in highp vec3 v_fuv;      /* (ACZ_VR_RQ_COCKPIT_WET_VUV: rq_cockpit_lwet2.vert) */
#endif
layout(location = 11) in vec3 v_sh;              /* the sky glint's half vector along U, V, n */
layout(location = 0) out vec4 o;

layout(constant_id = 0) const int WD = 1;        /* the beads and rivulets */
layout(constant_id = 1) const int WV = 1;        /* mist and film */
layout(constant_id = 2) const int WI = 1;        /* ice: frozen drops, rime */
layout(constant_id = 3) const int Q = 31;        /* ACZ_VR_RQ_COCKPIT_WET2 [test]: 1 the second rivulet layer, 2 the blur chain for film and rime, 4 the rime map */
layout(constant_id = 4) const int BQ = -4;       /* the drop tiles' LOD bias, quarters (ACZ_VR_RQ_COCKPIT_WET_BIAS) */

layout(set = 1, binding = 0) uniform highp sampler2DArray world;   /* the 1x feed: the frame's copy */
layout(set = 3, binding = 0) uniform sampler2D tda;                /* RG the lens (direction x min(rho,1) x steepness), B rho / 2, A presence draw */
layout(set = 3, binding = 1) uniform sampler2D tdb;                /* R half width (x 7.5 mm), G tail parameter, B the size draw, A 1 = sized by the drop intensity */
layout(set = 3, binding = 2) uniform sampler2D tnoise;             /* R film noise, G clumps, B rime mottling, A grain */
layout(set = 3, binding = 3) uniform sampler2D triv;               /* R lens across (signed, x 0.5 + 0.5), G rho / 2, B half width (x 3.2 mm), A presence, raised where the line fades out */
layout(set = 3, binding = 4) uniform sampler2D tice;               /* R feathers, G the fronds' mottling, B grain, A the crust's height (lower mips: the soft copy) */
layout(set = 3, binding = 5) uniform highp sampler2DArray blur;    /* the feed's small mip chain (film, rime) */
layout(std140, set = 2, binding = 2) uniform Fr {
    highp vec4 rot[3];
    highp vec4 head;
    highp vec4 proj;
    highp vec4 misc;
    highp vec4 xfx;
    highp vec4 xfy;
    highp vec4 l[3];
    highp vec4 r[4];
    highp vec4 dome[4];
    highp vec4 cloud;
    highp vec4 su, sv, sd;
    highp vec4 pack;
    highp vec4 tub;
    highp vec4 flp[4];
    highp vec4 flc[4];
    highp vec4 night;
    highp vec4 mir[3];
    highp vec4 mirg;
    highp vec4 eyesize;
    vec4 wa, wb, wc, wd, we, wf;
    highp vec4 uvk[2];
    vec4 wg;          /* x tail reach (of the baked tail), y size by intensity rk, z its exponent ex, w rivulet width by intensity */
    vec4 wh;          /* x smear, y the rain beads' share of the water against the bake's, z the fine beads' against the big ones' */
} F;

void main() {
#ifdef VUV
    highp vec2 uv0 = v_fuv.xy / v_fuv.z;                             /* (the interpolated position: no gl_FragCoord) */
#else
    highp vec2 uv0 = gl_ViewIndex == 0 ? vec2(gl_FragCoord.x * F.uvk[0].x + F.uvk[0].y, gl_FragCoord.y * F.uvk[1].x + F.uvk[1].y)
                                       : vec2(gl_FragCoord.x * F.uvk[0].z + F.uvk[0].w, gl_FragCoord.y * F.uvk[1].x + F.uvk[1].y);
#endif
    vec4 nn = texture(tnoise, v_nu);
    highp float px = length(fwidth(v_nu)) * 0.48 * 0.7;            /* a pixel's footprint on the glass, m (cockpit.frag: length(fwidth(p)) * 0.7) */
    vec2 lens = vec2(0.0);
    float m = 0.0, du = 0.0, sm = 0.0, avg = 0.97, drho = 1.0;     /* (drho: the top drop's rho, for the frozen drop's milky core and rim) */
    if (WD != 0) {
        float dens = v_sp.x * (0.3 + 0.7 * smoothstep(0.3, 0.75, nn.g));
        vec4 a = texture(tda, v_tc.xy, float(BQ) * 0.25);
        vec4 b = texture(tdb, v_tc.xy, float(BQ) * 0.25);
        float rho = a.b * 2.0, rs = b.r * 0.0075;
        dens *= b.a < 0.25 ? F.wh.y : (b.a < 0.75 ? F.wh.z : 1.0);        /* (the layers' own share of the water: A 0 the rain beads, 0.5 the fine ones, 1 the big ones) */
        if (b.a > 0.25 && F.wg.z != 3.0) {                          /* the drop intensity: bigger drops, more of the big ones (rain beads as they are) */
            float k = F.wg.y * (0.09 + 0.28 * pow(b.b, F.wg.z)) / (0.09 + 0.28 * b.b * b.b * b.b);
            rho /= k; rs *= k;
        }
        rho /= max(1.0 - smoothstep(0.7 * F.wg.x, F.wg.x, b.g), 0.05);   /* the tail by the speed: cut where this speed leaves it, narrowing to its end */
        float aa = max(px / max(rs, 1e-4), 1e-3), rpx = 1.0 / aa;
        float cover = clamp(1.6 * rpx - 0.6, 0.0, 1.0);            /* below ~1.5 px they fade out instead of flickering */
        aa = min(aa, 0.5);
        float m1 = (1.0 - smoothstep(1.0 - aa, 1.0, rho)) * cover * clamp((dens - a.a) * 16.0, 0.0, 1.0);
        float sm1 = clamp((rpx - 1.5) * 0.4, 0.0, 1.0);            /* under ~4 px of half width the lens fades to its average */
        vec4 r = texture(triv, v_tc.zw);
        float rr = r.g * 2.0 / F.wg.w, rw = r.b * 0.0032 * F.wg.w;
        float ar = max(px / max(rw, 1e-4), 1e-3), rpr = 1.0 / ar;
        float cr = clamp(1.6 * rpr - 0.6, 0.0, 1.0);
        ar = min(ar, 0.5);
        float mr = (1.0 - smoothstep(1.0 - ar, 1.0, rr)) * cr * clamp((v_sp.y - r.a) * 16.0, 0.0, 1.0);
        float lr = r.r;
        if (WI == 0 && (Q & 1) != 0) {                             /* a slower layer (water of another weight: a modern game's streak rates) */
            vec4 r2 = texture(triv, v_x.xy);
            float rr2 = r2.g * 2.0 / F.wg.w, rw2 = r2.b * 0.0032 * F.wg.w;
            float ar2 = max(px / max(rw2, 1e-4), 1e-3), rp2 = 1.0 / ar2;
            float cr2 = clamp(1.6 * rp2 - 0.6, 0.0, 1.0);
            ar2 = min(ar2, 0.5);
            float mr2 = (1.0 - smoothstep(1.0 - ar2, 1.0, rr2)) * cr2 * clamp((v_sp.y * 0.7 - r2.a) * 16.0, 0.0, 1.0);
            if (mr2 > mr) { mr = mr2; rr = rr2; rpr = rp2; lr = r2.r; r.a = r2.a; }
        }
        if (m1 >= mr) { lens = a.rg * 2.0 - 1.0; m = m1; du = b.b; sm = sm1; avg = 0.6; drho = rho; }
        else { drho = rr; lens = vec2((lr * 2.0 - 1.0) * 0.6, 0.0); m = mr; du = r.a; sm = clamp((rpr - 1.5) * 0.4, 0.0, 1.0); }
    }
    float sa2 = dot(lens, lens) * F.wb.w * F.wb.w;                 /* sine of the cap's slope, squared */
    /* the ray through the drop, cockpit.frag's own: t1 (in the water) out through the cap Nc = U lens.x + V lens.y - n sqrt(1 - sa2) (the glass's axes, sin = rho x the contact angle) */
    float cc = sqrt(max(1.0 - sa2, 0.0));
    float ti = (lens.x * v_fr.y + lens.y * v_fr.z) * F.wb.w - v_fr.w * cc;                       /* t1 . Nc */
    float s2 = 1.333 * sqrt(max(1.0 - ti * ti, 0.0));                                            /* sine of the way out (> 1: turned back: dark, more of it where the view is oblique) */
    float trt = 1.0 - smoothstep(0.93, 1.0, s2);
    float kk = sqrt(max(1.0 - 1.7769 * (1.0 - ti * ti), 0.0)) - 1.333 * ti;
    vec3 t2l = vec3(1.333 * v_fr.y + kk * lens.x * F.wb.w, 1.333 * v_fr.z + kk * lens.y * F.wb.w, 1.333 * v_fr.w - kk * cc);
    highp vec2 tg = (v_lens.xy * t2l.x + v_lens.zw * t2l.y + v_w.xy * t2l.z) / max(-(v_w.z * t2l.x + v_w.w * t2l.y + v_gl.z * t2l.z), 0.05);   /* its place in the frame image, less the centre */
    vec2 off = m > 0.0 ? (tg + vec2(F.proj.z * F.misc.x, F.proj.w * F.misc.y) * 0.5 - uv0) * (F.wb.z * (0.4 + 0.6 * sm)) : vec2(0.0);
    vec3 amb = v_amb.rgb;
    /* the terms under the drops that want a blurred world: the film, the rime */
    float fm = 0.0, lodb = 0.5;
    vec2 roff = vec2(0.0);
    vec4 ic = vec4(0.5);
    float ra = 0.0, rthick = 0.0, rcov = 0.0;
    if (WV != 0) {
        fm = smoothstep(v_sp.z - 0.1, v_sp.z + 0.14, nn.r) * v_sp.w;
        lodb = 0.5 + fm;
    }
    if (WI != 0 && F.wf.w > 0.0) {                                 /* rime: a ragged front from the cold sides, a crust grown along the flow */
        float th = F.wb.x, e = v_fr.x;
        if ((Q & 4) != 0) {
            ic = texture(tice, v_x.zw);
            float fe = e + 0.32 * (nn.g - 0.5) + 0.16 * (ic.g - 0.5);
            rcov = smoothstep(th - 0.02, th + 0.05, fe);
            float deep = smoothstep(th + 0.04, th + 0.4, fe);
            rthick = clamp(mix(0.55 * ic.a, 0.55 + 0.9 * (ic.a - 0.5), deep), 0.0, 1.0);
            roff = v_lens.xy * (ic.g - 0.5) * 0.06 + v_lens.zw * (ic.r - 0.5) * 0.04;     /* the world through the rough ice: displaced by its facets */
        } else {
            float fe = e + 0.32 * (nn.g - 0.5) + 0.16 * (nn.b - 0.5);
            rcov = smoothstep(th - 0.02, th + 0.05, fe);
            float deep = smoothstep(th + 0.04, th + 0.4, fe), h = 0.5 * nn.b + 0.5 * nn.a;
            rthick = clamp(mix(0.55 * h, 0.55 + 0.9 * (h - 0.5), deep), 0.0, 1.0);
        }
        ra = rcov * (0.07 + 0.45 * rthick) * mix(0.3, 1.0, e) * F.wa.w;
        lodb = max(lodb, 0.5 + 1.5 * rthick);
    }
    /* one tap: the world bent through a resolved drop, or the blurred world for the film and the rime; nothing where only veils lie */
    float rd = m * sm;
    float sms = F.wh.x > 0.0 ? smoothstep(0.45, 0.8, nn.g) * F.wh.x : 0.0;     /* the wet smear: the film shifts the view aft a little */
    vec3 tap = vec3(0.0);
    if (rd > 0.02) tap = texture(world, vec3(clamp(uv0 + off, vec2(0.0), vec2(1.0)), float(gl_ViewIndex))).rgb;
    else if (sms > 0.003) tap = texture(world, vec3(uv0 + v_lens.zw * (0.025 * sms), float(gl_ViewIndex))).rgb;
    else if (fm * F.wa.y > 0.003 || ra > 0.003) {
        tap = (Q & 2) != 0 ? textureLod(blur, vec3(uv0 + roff, float(gl_ViewIndex)), lodb).rgb
                           : texture(world, vec3(uv0 + roff, float(gl_ViewIndex))).rgb;
    }
    vec4 W = vec4(0.0);
    if (WV != 0) {
        float am = F.wa.x;
        W = vec4((amb * 1.4 + 0.08) * 0.25 * am, 0.25 * am);       /* the mist: a veil */
        float af = fm * F.wa.y;
        vec3 fc = mix(tap, amb * 1.5 + 0.1, 0.3);
        W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
    }
    if (F.we.z > 0.0) {                                            /* grime: only after debris */
        float ag = smoothstep(0.55, 0.9, nn.b) * F.we.z;
        W = vec4(vec3(0.16, 0.145, 0.125) * ag + W.rgb * (1.0 - 0.625 * ag), 0.625 * ag + W.a * (1.0 - 0.625 * ag));
    }
    if (sms > 0.003) {
        float as = sms * 0.85;
        W = vec4((tap * 0.97 + amb * 0.06 * sms) * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
    }
    if (F.we.w > 0.0) {                                            /* soot: only after smoke */
        float as = F.we.w * (0.08 + 0.14 * smoothstep(0.35, 0.8, nn.r));
        W = vec4((vec3(0.11, 0.10, 0.09) + amb * 0.12) * 0.6 * as + W.rgb * (1.0 - 0.78 * as), 0.78 * as + W.a * (1.0 - 0.78 * as));
    }
    if (m > 0.0) {
        /* past the critical angle the ray is turned back into the cockpit: the dark rim */
        float tr = mix(avg, trt, sm);
        float Tb = (1.0 - tr) * 0.3 + tr * 0.97 * (1.0 - sm), Tt = tr * 0.97 * sm;
        vec3 dc = tap * Tt + amb * 0.12 * (1.0 - tr);
        /* the sun: its small image through the lens (the ray leaving towards it), and off the turned-back rim (reflect(t1, Nc)), cockpit.frag's two terms */
        float gt = clamp(t2l.x * v_lh.y + t2l.y * v_lh.z + t2l.z * v_gl.x, 0.0, 1.0);
        float tl = (lens.x * v_lh.y + lens.y * v_lh.z) * F.wb.w - v_gl.x * cc;
        float gr = clamp(v_gl.y - 2.0 * ti * tl, 0.0, 1.0);
        float skyg = clamp(-(lens.x * v_sh.x + lens.y * v_sh.y) * F.wb.w + v_sh.z * cc, 0.0, 1.0);   /* the sky's soft highlight on the cap (N = -Nc) */
        skyg *= skyg; skyg *= skyg; skyg *= skyg; skyg *= skyg; skyg *= skyg; skyg *= skyg;
        dc += amb * 0.14 * skyg * sm;
        gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt;
        gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr;
        float gl = (gt * 1.5 * tr + gr * (1.0 - tr)) * v_amb.a * sm;
        float ad = m;
        if (WI != 0) {                                             /* frozen where it sat: small drops first, milky */
            float fz = smoothstep(0.0, 0.18, F.wb.y * 1.18 - du);
            float rim = smoothstep(0.72, 1.0, drho), core = 1.0 - smoothstep(0.05, 0.5, drho);    /* (the air the freezing pushed to the middle; a whiter rim) */
            float milk = clamp(0.55 + 0.2 * core + 0.35 * rim + 0.25 * (nn.a - 0.5), 0.0, 1.0);
            float Ti = (1.0 - milk) * (0.65 + 0.35 * Tb);
            vec3 ice = dc * ((1.0 - milk) * 0.35) + (amb * 1.55 + vec3(0.15, 0.16, 0.18)) * milk;
            Tb = mix(Tb, Ti, fz);
            dc = mix(dc, ice, fz);
            ad = mix(ad, m * min(0.55 + 0.15 * core + 0.3 * rim + 0.1 * nn.a, 1.0), fz);
            gl *= 1.0 - 0.7 * fz;
        }
        dc += F.wf.rgb * gl;
        ad *= F.wa.z;
        float at = ad * (1.0 - Tb);                                /* (the part of the world the drop replaces) */
        W = vec4(dc * ad + W.rgb * (1.0 - at), at + W.a * (1.0 - at));
    }
    if (ra > 0.0) {
        vec3 ic2 = amb * 1.45 + vec3(0.13, 0.14, 0.16);
        vec3 fc = mix(tap, ic2, 0.25 + 0.5 * rthick);
        W = vec4(fc * ra + W.rgb * (1.0 - ra), ra + W.a * (1.0 - ra));
    }
    o = vec4(v_gc.rgb + (1.0 - v_gc.a) * W.rgb, v_gc.a + (1.0 - v_gc.a) * W.a);
}
