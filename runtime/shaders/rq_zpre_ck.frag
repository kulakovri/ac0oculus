#version 450
#extension GL_EXT_multiview : require
/* rq_zpre_ck.frag -- PS2_RQ_ZPRE (rq_zpre.inc): the pre-pass's cockpit coverage. The Quest material's solid parts write alpha 16/255 (rq_cockpit.frag;
   the mark, RQZ_MARK) and the feed keeps it (rq_ds.frag -DNOZ): where the previous frame's feed holds the mark, this draw writes a cockpit depth (0.9975, in
   the cockpit's top-of-range band above the world's), so the clouds stop at the panel and the scene light's bands reject those pixels. */
layout(set = 0, binding = 0) uniform sampler2DArray feed;
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(feed, 0).xy);
    float a = textureLod(feed, vec3(uv, gl_ViewIndex), 0.0).a;
    if (abs(a - 16.0 / 255.0) > 5.5 / 255.0) discard;
    gl_FragDepth = 0.9975;
}
