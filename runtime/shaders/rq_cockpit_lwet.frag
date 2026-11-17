#version 450
#extension GL_EXT_multiview : require
/* The Quest cockpit's lean wet canopy (rq_cockpit.inc, ACZ_VR_RQ_COCKPIT_WET=lean). cockpit.frag's canopy_water marches 3D cells of hashes
   and value noise for every fragment (about 660 ALU on the Adreno): here those are baked once into tiling textures (the same bead and
   rivulet shapes, rqw_bake), and a fragment reads one bead tile, one rivulet tile, one noise tile and ONE tap of the world copy.
   Mist, film and frost take that same tap (the copy is small, so it is already the blur they asked for). mediump; each term a
   specialisation constant (one pipeline per combination that is active). */
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
#ifdef VUV
layout(location = 8) in highp vec3 v_fuv;       /* (ACZ_VR_RQ_COCKPIT_WET_VUV: rq_cockpit_lwet.vert) */
#endif
layout(location = 0) out vec4 o;

#ifdef NOSPEC
/* (ACZ_VR_RQ_COCKPIT_WET_NOSPEC [test], R-45b: no specialization constant reaches the driver; every term on) */
const int WD = 1, WV = 1, WI = 1, Q0 = 0, BQ = 0;
#else
layout(constant_id = 0) const int WD = 1;        /* the beads and rivulets */
layout(constant_id = 1) const int WV = 1;        /* mist and film */
layout(constant_id = 2) const int WI = 1;        /* ice: frozen drops, rime */
layout(constant_id = 3) const int Q0 = 0;        /* ACZ_VR_RQ_COCKPIT_WET_Q0=1 [test]: mist, grime, soot and the unresolved beads as blend factors (no world read) */
layout(constant_id = 4) const int BQ = 0;        /* ACZ_VR_RQ_COCKPIT_WET_BIAS [test]: the drop tile's LOD bias, in quarters */
#endif

layout(set = 1, binding = 0) uniform highp sampler2DArray world;   /* the 1x feed: the frame's copy */
layout(set = 3, binding = 0) uniform sampler2D tdrop;              /* RG lens (rho x direction x steepness), B cover, A presence */
layout(set = 3, binding = 1) uniform sampler2D triv;               /* R lens across, G cover, B presence */
layout(set = 3, binding = 2) uniform sampler2D tnoise;             /* R film noise, G clumps, B rime mottling, A grain */
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
    highp vec4 uvk[2];                                             /* eye pixel -> the frame image's uv, per view */
    vec4 wg, wh;                                                   /* (lean2's; wh.w: ACZ_VR_RQ_COCKPIT_WET_LIGHT, R-88) */
} F;

void main() {
#ifdef VUV
    highp vec2 uv0 = v_fuv.xy / v_fuv.z;                             /* (the interpolated position: no gl_FragCoord) */
#else
    highp vec2 uv0 = gl_ViewIndex == 0 ? vec2(gl_FragCoord.x * F.uvk[0].x + F.uvk[0].y, gl_FragCoord.y * F.uvk[1].x + F.uvk[1].y)
                                       : vec2(gl_FragCoord.x * F.uvk[0].z + F.uvk[0].w, gl_FragCoord.y * F.uvk[1].x + F.uvk[1].y);
#endif
    vec4 nn = texture(tnoise, v_nu);
    vec2 lens = vec2(0.0);
    float m = 0.0, du = 0.0, sm = 0.0, avg = 0.97;                 /* (sm: how well the lens resolves; avg: what it fades to) */
    if (WD != 0) {
        float dens = v_sp.x * (0.3 + 0.7 * smoothstep(0.3, 0.75, nn.g));
        vec4 d = BQ != 0 ? texture(tdrop, v_tc.xy, float(BQ) * 0.25) : texture(tdrop, v_tc.xy);
        vec4 r = texture(triv, v_tc.zw);
        float m1 = d.b * clamp((dens - d.a) * 16.0, 0.0, 1.0);
        float mr = r.g * clamp((v_sp.y - r.b) * 16.0, 0.0, 1.0);
        if (m1 >= mr) { lens = d.rg * 2.0 - 1.0; m = m1; du = fract(d.a * 7.13); sm = v_lh.w; avg = 0.6; }
        else { lens = vec2((r.r * 2.0 - 1.0) * 0.6, 0.0); m = mr; du = r.b; }     /* (a rivulet is a flat lens: cockpit.frag's steepness 0.6) */
    }
    float sa2 = dot(lens, lens) * F.wb.w * F.wb.w;                 /* sine of the cap's slope, squared */
    /* the lens turns the world over: towards the rim the view leaves through the far side (Snell through the cap, a fit) */
    float bend = (0.31 + 0.6 * sa2) * F.wb.z * (0.4 + 0.6 * sm);
    vec2 off = -(v_lens.xy * lens.x + v_lens.zw * lens.y) * bend * min(m * 8.0, 1.0);
    /* (ACZ_VR_RQ_COCKPIT_WET_LIGHT, R-88: kl, the in-cloud light (cloud_light, up to 0.65) as 0..1 x the switch, 0 out of cloud -- see below.
       In a cloud the lens shows the same fog it would without bending, and a long tap near the canopy bow read the dark frame (M10 105 s,
       the last failing drop): the offset shrinks to a quarter in cloud) */
    float kl = clamp(F.cloud.w * (1.0 / 0.65), 0.0, 1.0) * F.wh.w;
    off *= 1.0 - 0.75 * kl;
    /* (ACZ_VR_RQ_COCKPIT_WET_UNTINT, R-87: the tap is last frame's finished eye image, where cockpit.frag reads a world image of the scene
       alone. So (1) a lens near the image's edge read the feed outside this eye's crop -- black there --: the tap is kept inside the crop;
       (2) a lens near the canopy frame read the cockpit (the feed's alpha holds its coverage mark, RQZ_MARK 16/255, rq_zpre.inc): then the
       unshifted tap, the world behind this pixel; (3) this glass's own tint and light are already in the tap, so every drop, the mist and
       the film put the glass's layer on twice -- darker and, on a warm-tinted canopy (the Mirage 2000D's), red-brown: the glass's layer
       (v_gc, premultiplied) is taken out of the tap once) */
    highp vec2 uvt = uv0 + off;
    if (F.eyesize.w > 0.5) {
        highp float ux0 = gl_ViewIndex == 0 ? F.uvk[0].y : F.uvk[0].w, ux1 = ux0 + (gl_ViewIndex == 0 ? F.uvk[0].x : F.uvk[0].z) * F.eyesize.x;
        highp float vy0 = F.uvk[1].y, vy1 = vy0 + F.uvk[1].x * F.eyesize.y;
        uvt = clamp(uvt, vec2(min(ux0, ux1), min(vy0, vy1)), vec2(max(ux0, ux1), max(vy0, vy1)));
    }
    vec4 b4 = texture(world, vec3(uvt, float(gl_ViewIndex)));
    if (F.eyesize.w > 0.5 && abs(b4.a - 16.0 / 255.0) < 5.5 / 255.0) b4 = texture(world, vec3(uv0, float(gl_ViewIndex)));
    vec3 base = b4.rgb;
    if (F.eyesize.w > 0.5) base = clamp((base - v_gc.rgb) / max(1.0 - v_gc.a, 0.1), 0.0, 1.0);
    vec3 amb = v_amb.rgb;
    /* (ACZ_VR_RQ_COCKPIT_WET_LIGHT, R-88, the user's M10 flight 105 / 116-120 s: "strange drops", "black arrows" -- in cloud each drop was
       a dark red-brown dash: its rim, the ray turned back past the critical angle, showed the dim cockpit interior (base x 0.3 + amb x
       0.12, cockpit.frag's canopy_water) and a lens under ~4 px faded to that rim's share, 0.6. Inside a cloud the fog's light comes
       from every side and the user's rule is that water on the glass is light and refracting, never darker than the cloud behind it:
       there the rim takes the cloud's light (a little brighter: the fog reflected at its grazing edge) and the small lens fades to the
       cloud seen through it. kl: the in-cloud light (cloud_light, up to 0.65) as 0..1 x the switch; 0 out of cloud and with the switch 0) */
    vec4 W = vec4(0.0);
    if (WV != 0) {
        float am = F.wa.x;
        /* (WET_LIGHT, kl: in cloud a veil of mist or condensation scatters the fog's own light -- never darker than the cloud behind it;
           the user's M10 105 s "blotches": the film's noise as darker patches) */
        vec3 vm = amb * 1.4 + 0.08, vf = amb * 1.5 + 0.1;
        vm = mix(vm, max(vm, base * 1.04), kl); vf = mix(vf, max(vf, base * 1.04), kl);
        W = Q0 != 0 ? vec4((amb * 1.4 + 0.08) * 0.25 * am, 0.25 * am) : vec4(mix(base, vm, 0.25) * am, am);
        float fm = smoothstep(v_sp.z - 0.1, v_sp.z + 0.14, nn.r) * v_sp.w;
        float af = fm * F.wa.y;
        vec3 fc = mix(base, vf, 0.3);
        W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
    }
    if (F.we.z > 0.0) {                                            /* grime: only after debris */
        float ag = smoothstep(0.55, 0.9, nn.b) * F.we.z;
        if (Q0 != 0) W = vec4(vec3(0.16, 0.145, 0.125) * ag + W.rgb * (1.0 - 0.625 * ag), 0.625 * ag + W.a * (1.0 - 0.625 * ag));
        else
        W = vec4(mix(base * 0.75, vec3(0.32, 0.29, 0.25), 0.5) * ag + W.rgb * (1.0 - ag), ag + W.a * (1.0 - ag));
    }
    if (F.we.w > 0.0) {                                            /* soot: only after smoke */
        float as = F.we.w * (0.08 + 0.14 * smoothstep(0.35, 0.8, nn.r));
        if (Q0 != 0) W = vec4((vec3(0.11, 0.10, 0.09) + amb * 0.12) * 0.6 * as + W.rgb * (1.0 - 0.78 * as), 0.78 * as + W.a * (1.0 - 0.78 * as));
        else
        W = vec4(mix(base * 0.55, vec3(0.11, 0.10, 0.09) + amb * 0.12, 0.6) * as + W.rgb * (1.0 - as), as + W.a * (1.0 - as));
    }
    if (m > 0.0) {
        /* past the critical angle the ray is turned back into the cockpit: the dark rim (in cloud: the cloud's light, kl) */
        float tr = mix(mix(avg, 1.0, kl), 1.0 - smoothstep(0.487, 0.5625, sa2), sm);
        vec3 dc = mix(mix(base * 0.3 + amb * 0.12, base * 1.06, kl), base * mix(0.97, 1.02, kl), tr);
        float Tb = 0.0, Tt = 1.0;                                  /* (Q0: what of dc is the sharp world behind (a blend factor) and what the lens's tap) */
        if (Q0 != 0) {
            Tb = (1.0 - tr) * 0.3 + tr * 0.97 * (1.0 - sm);
            Tt = tr * 0.97 * sm;
            dc = base * Tt + amb * 0.12 * (1.0 - tr);
        }
        /* the sun: its small image through the lens (the ray leaving towards it), and off the turned-back rim (cockpit.frag's two terms) */
        float d2 = bend * bend * dot(lens, lens);               /* (cos and sin of the deviation: unit vectors, or the peaks spread) */
        float gt = clamp(v_lh.x * (1.0 - 0.5 * d2) - (lens.x * v_lh.y + lens.y * v_lh.z) * bend, 0.0, 1.0);
        float gr = clamp(v_fr.y * sqrt(max(1.0 - sa2, 0.0)) + (lens.x * v_fr.z + lens.y * v_fr.w) * F.wb.w, 0.0, 1.0);
        gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt; gt *= gt;
        gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr; gr *= gr;
        float gl = (gt * 1.5 * tr + gr * (1.0 - tr)) * v_amb.a * sm;        /* (x sm, as cockpit.frag: no glints on drops too small to resolve) */
        float ad = m;
        if (WI != 0) {                                             /* frozen where it sat: small drops first, milky */
            float fz = smoothstep(0.0, 0.18, F.wb.y * 1.18 - du);
            vec3 ice = mix(mix(base, dc, 0.35), amb * 1.55 + vec3(0.15, 0.16, 0.18), 0.65);
            if (Q0 != 0) {                                         /* (the ice's third of the base is the sharp world: a factor too) */
                float Ti = 0.35 * (0.65 + 0.35 * Tb);
                ice = dc * 0.1225 + (amb * 1.55 + vec3(0.15, 0.16, 0.18)) * 0.65;
                Tb = mix(Tb, Ti, fz);
            }
            dc = mix(dc, ice, fz);
            ad = mix(ad, m * 0.7, fz);
            gl *= 1.0 - 0.7 * fz;                                  /* (ice: a third of the lens's glint, as cockpit.frag) */
        }
        dc += F.wf.rgb * gl;
        ad *= F.wa.z;
        float at = ad * (1.0 - Tb);                                /* (the part of the world the drop replaces) */
        W = vec4(dc * ad + W.rgb * (1.0 - at), at + W.a * (1.0 - at));
    }
    if (WI != 0 && F.wf.w > 0.0) {                                 /* rime: a ragged front from the cold sides, a mottled crust */
        float th = F.wb.x, e = v_fr.x;
        float fe = e + 0.32 * (nn.g - 0.5) + 0.16 * (nn.b - 0.5);
        float cov = smoothstep(th - 0.02, th + 0.05, fe);
        float deep = smoothstep(th + 0.04, th + 0.4, fe);
        float h = 0.5 * nn.b + 0.5 * nn.a;
        float thick = clamp(mix(0.55 * h, 0.55 + 0.9 * (h - 0.5), deep), 0.0, 1.0);
        float af = cov * (0.07 + 0.45 * thick) * mix(0.3, 1.0, e) * F.wa.w;
        vec3 ic = amb * 1.45 + vec3(0.13, 0.14, 0.16);
        vec3 fc = mix(base, ic, 0.25 + 0.5 * thick);
        W = vec4(fc * af + W.rgb * (1.0 - af), af + W.a * (1.0 - af));
    }
    o = vec4(v_gc.rgb + (1.0 - v_gc.a) * W.rgb, v_gc.a + (1.0 - v_gc.a) * W.a);
}
