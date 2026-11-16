#version 460
// Bloom chain steps on the HDR scene: threshold + 4x downsample, separable blur, plain 4x downsample.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 params;        // x = mode (0 threshold-downsample, 1 blur horizontal, 2 blur vertical, 3 downsample), y = threshold
} pc;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 px = 1.0 / vec2(textureSize(src, 0));
    int mode = int(pc.params.x);
    vec3 c;
    if (mode == 0 || mode == 3) {
        // 4x downsample: four bilinear taps cover the 4x4 source block
        c = (texture(src, vUv + px * vec2(-1.0, -1.0)).rgb + texture(src, vUv + px * vec2(1.0, -1.0)).rgb +
             texture(src, vUv + px * vec2(-1.0, 1.0)).rgb + texture(src, vUv + px * vec2(1.0, 1.0)).rgb) * 0.25;
        if (mode == 0) {
            c = min(c, vec3(24.0));                     // no single pixel may flood the frame
            float l = max(c.r, max(c.g, c.b));
            c *= max(l - pc.params.y, 0.0) / max(l, 1e-4);
        }
    } else {
        vec2 d = mode == 1 ? vec2(px.x, 0.0) : vec2(0.0, px.y);
        c = texture(src, vUv).rgb * 0.227027;
        c += (texture(src, vUv + d * 1.384615).rgb + texture(src, vUv - d * 1.384615).rgb) * 0.316216;
        c += (texture(src, vUv + d * 3.230769).rgb + texture(src, vUv - d * 3.230769).rgb) * 0.070270;
    }
    outColor = vec4(c, 1.0);
}
