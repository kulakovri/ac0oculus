#version 450
/* The Quest canopy's dust specks and water-spot rings (rq_glass_dust.vert): the profile across the quad, additive
   (premultiplied, alpha 0: the world behind is not dimmed). No position read (R-45). */
precision mediump float;
layout(location = 0) in vec3 v_col;
layout(location = 1) in vec2 v_c;
layout(location = 2) in vec4 v_r;
layout(location = 0) out vec4 o;

void main() {
    float r = length(v_c);
    float k;
    if (v_r.x <= 0.0) k = 1.0 - smoothstep(0.35, 1.0, r);
    else {
        float prof = 1.0 - smoothstep(0.0, v_r.x, abs(r - 1.0));
        vec2 tt = r > 1e-3 ? vec2(-v_c.y, v_c.x) / r : vec2(1.0, 0.0);
        float gq = dot(v_r.zw, tt);
        k = prof * (0.3 + 0.7 * exp(-gq * gq * 150.0));
    }
    o = vec4(min(v_col * k, vec3(0.25)), 0.0);
}
