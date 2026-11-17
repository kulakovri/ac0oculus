#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_multiview : require
/* The Quest cockpit material's fragment stage (rq_cockpit.inc, ACZ_VR_COCKPIT_MATERIAL=quest): one texture read through
   a non-uniform index into the model's texture table, one hardware 2x2 PCF shadow read, all lighting interpolated from
   the vertex stage. Target by the counters: at most 20 ALU and 2 texture reads a fragment. */
precision mediump float;
precision mediump int;
#ifdef RQC_MP
layout(location = 0) in mediump vec2 v_uv;       /* (ACZ_VR_RQ_COCKPIT_DBG 8: what highp interpolants cost; the picture degrades) */
#else
layout(location = 0) in highp vec2 v_uv;
#endif
layout(location = 1) in mediump vec3 v_base;
layout(location = 2) in mediump vec3 v_key;
layout(location = 3) in mediump vec3 v_spec;
#ifdef RQC_MP
layout(location = 4) in mediump vec3 v_sm;
#else
layout(location = 4) in highp vec3 v_sm;
#endif
layout(location = 5) flat in uint v_tex;
layout(location = 6) in mediump vec3 v_n1;
layout(location = 7) in mediump vec3 v_n2;
layout(location = 8) in mediump vec4 v_rpw;     /* NIGHT 2 (R-85): the position from the head; w a self-lit page's light */
layout(location = 0) out vec4 o;

layout(set = 0, binding = 0) uniform sampler2DArray texs[1024];
layout(set = 1, binding = 0) uniform sampler2DShadow smap;

layout(constant_id = 0) const int CUT = 0;      /* 1: alpha-tested cut-out parts (no pre-pass here: their own draw) */
layout(constant_id = 1) const int NIGHT = 0;    /* 1: night instrument lighting (a second pipeline used only at night); 2: its panel area per
                                                   fragment (ACZ_VR_RQ_COCKPIT_NIGHT_FS, R-85) */
layout(constant_id = 2) const int SM = 1;       /* 0: the shadow map is switched off (ACZ_VR_COCKPIT_SHADOWS=0) */
layout(constant_id = 3) const int DBGM = 0;     /* ACZ_VR_RQ_COCKPIT_DBG: 1 a constant colour, 2 one fixed texture slot */

void main() {
    if ((DBGM & 1) != 0) { o = vec4(0.4, 0.4, 0.45, 1.0); return; }
    mediump vec4 t = (DBGM & 2) != 0 ? texture(texs[1], vec3(v_uv, 0.0)) : texture(texs[nonuniformEXT(v_tex)], vec3(v_uv, 0.0));
    if (CUT != 0 && t.a < 0.5) discard;
    mediump float s = SM != 0 ? texture(smap, v_sm) : 1.0;
    mediump vec3 c = t.rgb * (v_base + v_key * s) + v_spec * s;
    if (NIGHT != 0) {
        /* the markings: the texel against its own 8x8 mean (3 mip levels up), as cockpit.frag */
        mediump vec3 t3 = texture(texs[nonuniformEXT(v_tex)], vec3(v_uv, 0.0), 3.0).rgb;   /* (bias 3, as cockpit.frag's SAMPLE_BIAS) */
        mediump float l0 = dot(t.rgb, vec3(0.299, 0.587, 0.114)), l3 = dot(t3, vec3(0.299, 0.587, 0.114));
        mediump float sat = max(t.r, max(t.g, t.b)) - min(t.r, min(t.g, t.b));
        mediump float mk = smoothstep(0.35, 0.6, l0) * smoothstep(0.10, 0.28, l0 - l3) * (1.0 - smoothstep(0.12, 0.35, sat));
        if (NIGHT == 2) {
            mediump vec3 rp = v_rpw.xyz;
            mediump float area = (1.0 - smoothstep(-0.12, -0.06, rp.y)) * (1.0 - smoothstep(1.1, 1.4, length(rp)))
                   * max(1.0 - smoothstep(-0.38, -0.28, rp.z), smoothstep(0.18, 0.26, abs(rp.x)))
                   * (1.0 - smoothstep(0.15, 0.35, rp.z));
            c += t.rgb * ((v_n1 + v_n2 * mk) * area + v_rpw.w);
        } else c += t.rgb * (v_n1 + v_n2 * mk);
    }
    o = vec4(c, 16.0 / 255.0);    /* (alpha: the coverage mark of PS2_RQ_ZPRE, rq_zpre.inc; written only when that switch is on) */
}
