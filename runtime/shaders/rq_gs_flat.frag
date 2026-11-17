#version 450
/* rq_gs_flat: PS2_RQ_LEAN's variant for the untextured GS draws of the eye-sized pass (the HUD's frames, the radar's masks and
   grid, lines): gs.frag's untextured path only -- fog, alpha test, dual-source blend output. Compiled with gs.frag's
   push constants (-DRQ). */
#ifdef MP
precision mediump float;   /* (PS2_RQ_MP: relaxed precision; coordinates, depth and flags stay highp, named below) */
#endif
layout(location = 0) in vec4 v_color;
layout(location = 2) in float v_fog;
#ifdef RQCUT
layout(location = 4) in highp vec4 v_cut;  /* PS2_RQ_CLOUDS_CUT (rq_cut.glsl, gs.vert): the cloud in front of this vertex, rgb L_d, a T_d */
layout(location = 5) flat in highp vec4 v_cutf;   /* (round 2) the provoking vertex's (mv_flags bit 26) */
#endif
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;
layout(push_constant) uniform PC {
    highp vec2  inv_size;
    highp vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
} pc;
void main() {
    vec4 c = v_color;
    uint f = pc.flags.x;
    c.rgb *= 128.0 / 255.0;
    if (((f >> 16) & 1u) != 0u) c.rgb = mix(pc.fogcol.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));
    bool apass = true;
    if (((f >> 3) & 1u) != 0u) {
        uint atst = (f >> 4) & 7u;
        float aref = float((f >> 8) & 0xFFu) / 128.0;
        bool pass;
        if      (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a <  aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = abs(c.a - aref) < (0.5 / 128.0);
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a >  aref;
        else                 pass = abs(c.a - aref) >= (0.5 / 128.0);
        apass = pass;
        bool afonly = ((f >> 20) & 1u) != 0u, afkeep = ((f >> 18) & 1u) != 0u;
        if (afonly ? pass : (!pass && afkeep)) discard;
    }
    if (((f >> 30) & 1u) != 0u) out_blend = vec4(vec3(clamp(c.a, 0.0, 1.0)), apass ? 1.0 : 0.0);
    else out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
    if (((f >> 21) & 1u) != 0u) c.a = max(c.a, 1.0);
#ifdef RQCUT
    {   highp vec4 rqc = v_cut;                     /* (PS2_RQ_CLOUDS_CUT; clamped; untextured draws: no cards) */
        c.rgb = c.rgb * clamp(rqc.a, 0.0, 1.0) + max(rqc.rgb, vec3(0.0)); }
#endif
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}
