#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_multiview : require
/* The Quest cockpit's dry glass (rq_cockpit.inc): premultiplied, one texture read (through the texture table), the
   lighting per vertex. Drawn only on the triangles the CPU kept for this eye and sun. */
precision mediump float;
precision mediump int;
layout(location = 0) in highp vec2 v_uv;
layout(location = 1) in vec3 v_li;
layout(location = 2) in vec3 v_add;
layout(location = 3) in float v_fa;
layout(location = 4) in float v_ca;
layout(location = 5) flat in uint v_tex;
layout(location = 0) out vec4 o;
layout(set = 0, binding = 0) uniform sampler2DArray texs[1024];

void main() {
    vec4 t = texture(texs[nonuniformEXT(v_tex)], vec3(v_uv, 0.0));
    float ca = v_ca * t.a;
    float a = clamp(ca + (1.0 - ca) * v_fa, 0.0, 1.0);
    o = vec4(t.rgb * v_li * ca + v_add, a);
}
