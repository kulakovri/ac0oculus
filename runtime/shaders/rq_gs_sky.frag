#version 450
/* rq_gs_sky: PS2_RQ_SKY: the eye pass's opaque untextured GS draws without fog, FBA or a real alpha test (the sky dome and the background
   quad under it): gs.frag's result for them, nothing else. Blending is off for these draws, so no second output. */
precision mediump float;
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = clamp(vec4(v_color.rgb * (128.0 / 255.0), v_color.a), vec4(0.0), vec4(2.0));
}
