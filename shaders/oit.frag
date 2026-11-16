#version 460
// Transparency resolve: every blended fragment of the frame was stored per pixel with its depth (scene.frag, -DOIT).
// Here each pixel's fragments are sorted by distance from the eye and composited far to near over the lit scene.
// Nothing else decides the order: no object lists, draw groups or per-object distances.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec4 outColor;     // rgb = composited fragments (premultiplied), a = what remains of the background

void main() {
    uint n = imageLoad(oitHeads, ivec2(gl_FragCoord.xy)).r;
    if (n == 0u) discard;
    const int K = 24;
    vec4 col[K];
    float dep[K];
    int count = 0;
    while (n != 0u && count < K) {
        OitNode nd = oitNodes[n - 1u];
        col[count] = vec4(unpackHalf2x16(nd.rg), unpackHalf2x16(nd.ba));
        dep[count] = nd.depth;
        count++;
        n = nd.next;
    }
    for (int i = 1; i < count; i++) {       // insertion sort, farthest first
        vec4 c = col[i]; float d = dep[i];
        int j = i - 1;
        while (j >= 0 && dep[j] < d) { col[j + 1] = col[j]; dep[j + 1] = dep[j]; j--; }
        col[j + 1] = c; dep[j + 1] = d;
    }
    vec3 acc = vec3(0.0);
    float through = 1.0;
    for (int i = 0; i < count; i++) {
        acc = col[i].rgb * col[i].a + acc * (1.0 - col[i].a);
        through *= 1.0 - col[i].a;
    }
    outColor = vec4(acc, through);          // blended as: result = acc + background * through
}
