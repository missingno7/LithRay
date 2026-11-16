#version 460
// Lighting resolve for the fully ray-traced mode. The opaque pass wrote albedo and a noisy per-pixel light estimate
// (direct lamp rays + one indirect bounce). Here the estimate is filtered across neighbours on the same surface
// (depth + normal weights), blended with the reprojected history, multiplied back onto the albedo and fogged.
// Pixels the opaque pass shaded completely (light.a == 0: sky, emissive, view model, raster mode) pass through.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;   // albedo, or the final colour when light.a == 0
layout(set = 0, binding = 1) uniform sampler2D sceneAux;     // rg motion, b ao, a packed normal
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D sceneLight;   // rgb light estimate, a = 1 when it must be composed
layout(set = 0, binding = 4) uniform sampler2D lightHistory; // rgb accumulated INDIRECT light, a = linear depth / far (< 0: none)
layout(set = 0, binding = 5) uniform sampler2D sceneIndirect; // rgb noisy bounce light of this frame, a = 1 + in-scattered light
layout(set = 0, binding = 6) uniform sampler2D directHistory; // rgb accumulated direct light, a = accumulated in-scattering

layout(push_constant) uniform PC {
    vec4 fogColor;      // rgb, a = enabled
    vec4 fogParams;     // fog near, fog far, z near, z far
    vec4 params;        // x = history valid, y = history weight, z = tan(fov x / 2), w = tan(fov y / 2)
    vec4 viewport;      // scene viewport in uv: x, y, w, h
    vec4 look;          // x = modern look (linear HDR, no shoulder), y = fullbright (emissive) level, z = direct-light history weight
    vec4 scatterTint;   // rgb: colour of the in-scattered light
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outLit;
layout(location = 1) out vec4 outHistory;
layout(location = 2) out vec4 outDirect;

float linearDepth(float raw) {
    float d = max((raw - 0.02) / 0.98, 0.0);
    return pc.fogParams.z * pc.fogParams.w / (pc.fogParams.w - d * (pc.fogParams.w - pc.fogParams.z));
}
vec3 unpackNormal(float packed) {
    float y = floor(packed / 4096.0), x = packed - y * 4096.0;
    vec2 e = vec2(x, y) / 4095.0 * 2.0 - 1.0;
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t; n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    vec4 albedo = texture(sceneColor, vUv);
    vec4 raw = texture(sceneLight, vUv);
    if (raw.a < 0.5) {
        outLit = vec4(albedo.rgb, 1.0);
        outHistory = vec4(0.0, 0.0, 0.0, -1.0);
        outDirect = vec4(0.0);
        return;
    }
    vec4 aux = texture(sceneAux, vUv);
    float lin = linearDepth(texture(sceneDepth, vUv).r);
    vec3 n = unpackNormal(aux.a);

    // ---- spatial: 5x5 taps, 2 pixels apart, only across the same surface
    vec2 px = 1.0 / vec2(textureSize(sceneIndirect, 0));
    vec3 sum = vec3(0.0); float wsum = 0.0, scatter = 0.0;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            vec2 uv = vUv + vec2(x, y) * px * 2.0;
            vec4 l = texture(sceneIndirect, uv);
            if (l.a < 0.5) continue;
            float d = linearDepth(texture(sceneDepth, uv).r);
            vec3 nn = unpackNormal(texture(sceneAux, uv).a);
            float w = exp(-abs(d - lin) / max(lin * 0.015, 0.05)) * pow(max(dot(n, nn), 0.0), 24.0);
            w *= (x == 0 && y == 0) ? 1.0 : 0.7;
            sum += min(l.rgb, vec3(4.0)) * w; wsum += w;
            scatter += min(l.a - 1.0, 2.0) * w;
        }
    vec3 light = wsum > 1e-4 ? sum / wsum : min(texture(sceneIndirect, vUv).rgb, vec3(4.0));
    scatter = wsum > 1e-4 ? scatter / wsum : 0.0;

    // direct light: this frame's exact value, its 3x3 range (history is clamped to it, which stops ghosting wherever
    // the light is not in a penumbra) and the reprojected accumulation that turns one jittered shadow ray into a soft edge
    vec3 direct = raw.rgb, dmin = raw.rgb, dmax = raw.rgb;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec4 r = texture(sceneLight, vUv + vec2(x, y) * px);
            if (r.a < 0.5) continue;
            dmin = min(dmin, r.rgb); dmax = max(dmax, r.rgb);
        }

    // ---- temporal: the accumulated light of the same surface point one frame ago
    vec2 prevUv = vUv - aux.rg;
    if (pc.params.x > 0.5 && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
        vec4 h = texture(lightHistory, prevUv);
        float hl = h.a * pc.fogParams.w;
        if (h.a >= 0.0 && abs(hl - lin) < max(lin * 0.06, 1.5)) {
            light = mix(light, h.rgb, pc.params.y);
            vec4 dh = texture(directHistory, prevUv);
            direct = mix(direct, clamp(dh.rgb, dmin, dmax), pc.look.z);
            scatter = mix(scatter, dh.a, pc.params.y);
        }
    }
    outHistory = vec4(light, lin / pc.fogParams.w);
    outDirect = vec4(direct, scatter);
    // direct light (lamps, shadows, dynamic lights, reflections) is never blurred across pixels
    light += direct;

    // The original bake clamps at white; summed physical lamps do not. A soft shoulder above mid-grey keeps bright
    // areas from flattening out while leaving shadows and dim areas linear.
    float l = dot(light, vec3(0.299, 0.587, 0.114));
    if (pc.look.x < 0.5 && l > 0.5) light *= (0.5 + (l - 0.5) / (1.0 + (l - 0.5) * 1.1)) / l;
    vec3 color = albedo.rgb * mix(light, vec3(pc.look.y), albedo.a) + pc.scatterTint.rgb * scatter;     // albedo.a: the texture's fullbright (emissive) layer
    if (pc.fogColor.a > 0.5) {
        vec2 ndc = ((vUv - pc.viewport.xy) / pc.viewport.zw) * 2.0 - 1.0;
        float dist = lin * length(vec3(ndc.x * pc.params.z, ndc.y * pc.params.w, 1.0));
        color = mix(color, pc.look.x > 0.5 ? pow(pc.fogColor.rgb, vec3(2.2)) : pc.fogColor.rgb, clamp((dist - pc.fogParams.x) / max(pc.fogParams.y - pc.fogParams.x, 1.0), 0.0, 1.0));
    }
    outLit = vec4(color, 1.0);
}
