#version 460
// Final pass. Input: the lit scene (linear HDR in the modern look, display-referred in the faithful look).
// Ambient occlusion (lightmap mode), bloom, exposure, tone mapping, output encoding (SDR gamma with dithering, or
// HDR10 / PQ), then the engine's 2D layer (menus, HUD, console) on top.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;   // the lit scene (after the lighting resolve and the translucent pass)
layout(set = 0, binding = 1) uniform sampler2D sceneAux;     // rg motion, b raw AO
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D aoHistory;    // r = accumulated AO, g = linear depth / far
layout(set = 0, binding = 4) uniform sampler2D overlay;      // rgb + coverage
layout(set = 0, binding = 5) uniform sampler2D bloomNear;    // 1/4 resolution, blurred
layout(set = 0, binding = 6) uniform sampler2D bloomWide;    // 1/16 resolution, blurred

layout(push_constant) uniform PC {
    vec4 overlayRect;   // mode area inside the output, in uv: x, y, w, h
    vec4 lightAdd;      // rgb add, a = has scene
    vec4 params;        // x = ao enabled, y = history valid, z = near, w = far
    vec4 params2;       // x = exposure, y = debug view, z = the 2D layer has content
    vec4 look;          // x = modern look (linear HDR input, tone mapped), y = bloom strength, z = HDR10 output, w = paper white (nits)
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;

// the world draws into depth [0.02, 1]; the view model owns [0, 0.02]
float linearDepth(float raw) { float d = max((raw - 0.02) / 0.98, 0.0); return pc.params.z * pc.params.w / (pc.params.w - d * (pc.params.w - pc.params.z)); }

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
// ACES filmic curve (Narkowicz fit): keeps contrast in the mids, rolls highlights off instead of clipping
vec3 tonemap(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 pq(vec3 nits) {
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(0.1593017578125));
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}
// display-linear (1.0 = paper white) -> the swapchain's encoding
vec3 encode(vec3 lin) {
    if (pc.look.z > 0.5) {
        const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);
        return pq(to2020 * lin * pc.look.w);
    }
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2));
}
float dither(vec2 p) {      // triangular noise around zero: hides banding in smooth gradients
    float a = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453), b = fract(sin(dot(p + 17.0, vec2(39.3468, 11.135))) * 24634.6345);
    return (a + b - 1.0);
}

void main() {
    vec3 lin = vec3(0.0);       // display-linear scene
    float ao = 1.0, ld = 0.0;
    bool modern = pc.look.x > 0.5;
    if (pc.lightAdd.a > 0.5) {
        vec3 scene = texture(sceneColor, vUv).rgb;
        if (pc.params.x > 0.5) {
            float depth = texture(sceneDepth, vUv).r;
            ld = linearDepth(depth) / pc.params.w;
            // depth-aware 5x5 cross filter of the raw 2-ray AO
            vec2 px = 1.0 / vec2(textureSize(sceneAux, 0));
            float sum = 0.0, wsum = 0.0;
            for (int y = -2; y <= 2; y++)
                for (int x = -2; x <= 2; x++) {
                    vec2 uv = vUv + vec2(x, y) * px;
                    float d = linearDepth(texture(sceneDepth, uv).r) / pc.params.w;
                    float w = exp(-abs(d - ld) / max(ld * 0.02, 1e-6));
                    sum += texture(sceneAux, uv).b * w; wsum += w;
                }
            ao = sum / max(wsum, 1e-4);
            // temporal accumulation along the motion vector, rejected on depth mismatch
            vec4 aux = texture(sceneAux, vUv);
            vec2 prevUv = vUv - aux.rg;
            if (pc.params.y > 0.5 && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                vec4 h = texture(aoHistory, prevUv);
                float ok = step(abs(h.g - ld), max(ld * 0.08, 2e-4));
                ao = mix(ao, h.r, 0.88 * ok);
            }
            if (depth >= 1.0) ao = 1.0;
            scene *= mix(1.0, ao, 0.7);
        }
        if (modern) {
            vec3 hdr = scene;
            if (pc.look.y > 0.0) hdr += (texture(bloomNear, vUv).rgb * 0.55 + texture(bloomWide, vUv).rgb * 0.45) * pc.look.y;
            hdr += toLinear(pc.lightAdd.rgb);                           // screen flashes
            hdr *= pc.params2.x;
            // SDR: the filmic curve maps to [0, 1]. HDR10: the same curve below paper white, highlights kept up to ~5x.
            lin = pc.look.z > 0.5 ? mix(tonemap(hdr), hdr * 0.8, smoothstep(0.6, 2.5, max(hdr.r, max(hdr.g, hdr.b)))) : tonemap(hdr);
        } else {
            lin = toLinear((scene + pc.lightAdd.rgb) * pc.params2.x);    // the faithful look stays display-referred
        }
        if (int(pc.params2.y) == 3) lin = vec3(ao);
    }
    outHistory = vec4(ao, ld, 0.0, 1.0);

    vec2 ouv = (vUv - pc.overlayRect.xy) / pc.overlayRect.zw;
    if (all(greaterThanEqual(ouv, vec2(0.0))) && all(lessThanEqual(ouv, vec2(1.0)))) {
        // an empty layer is not uploaded: the image still holds the last non-empty one (a menu) and must not be shown
        vec4 o = pc.params2.z > 0.5 ? texture(overlay, ouv) : vec4(0.0);
        lin = mix(lin, toLinear(o.rgb), o.a);
    } else if (pc.lightAdd.a < 0.5)
        lin = vec3(0.0);
    vec3 enc = encode(lin);
    enc += dither(gl_FragCoord.xy) * (pc.look.z > 0.5 ? 0.5 / 1023.0 : 1.0 / 255.0);
    outColor = vec4(enc, 1.0);
}
