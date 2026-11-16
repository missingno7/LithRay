#version 460
// Resolve + present: temporally accumulated, depth-aware filtered ambient occlusion applied to the scene, the
// camera's light add (screen flashes), then the engine's 2D layer (menus, HUD, console) on top.
layout(set = 0, binding = 0) uniform sampler2D sceneColor;   // the lit scene (after the lighting resolve and the translucent pass)
layout(set = 0, binding = 1) uniform sampler2D sceneAux;     // rg motion, b raw AO
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D aoHistory;    // r = accumulated AO, g = linear depth / far
layout(set = 0, binding = 4) uniform sampler2D overlay;      // rgb + coverage

layout(push_constant) uniform PC {
    vec4 overlayRect;   // mode area inside the output, in uv: x, y, w, h
    vec4 lightAdd;      // rgb add, a = has scene
    vec4 params;        // x = ao enabled, y = history valid, z = near, w = far
    vec4 params2;       // x = exposure, y = debug view
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;

// the world draws into depth [0.02, 1]; the view model owns [0, 0.02]
float linearDepth(float raw) { float d = max((raw - 0.02) / 0.98, 0.0); return pc.params.z * pc.params.w / (pc.params.w - d * (pc.params.w - pc.params.z)); }

void main() {
    vec3 color = vec3(0.0);
    float ao = 1.0, lin = 0.0;
    if (pc.lightAdd.a > 0.5) {
        vec4 scene = texture(sceneColor, vUv);
        color = scene.rgb;
        if (pc.params.x > 0.5) {
            float depth = texture(sceneDepth, vUv).r;
            lin = linearDepth(depth) / pc.params.w;
            // depth-aware 5x5 cross filter of the raw 2-ray AO
            vec2 px = 1.0 / vec2(textureSize(sceneAux, 0));
            float sum = 0.0, wsum = 0.0;
            for (int y = -2; y <= 2; y++)
                for (int x = -2; x <= 2; x++) {
                    vec2 uv = vUv + vec2(x, y) * px;
                    float d = linearDepth(texture(sceneDepth, uv).r) / pc.params.w;
                    float w = exp(-abs(d - lin) / max(lin * 0.02, 1e-6));
                    sum += texture(sceneAux, uv).b * w; wsum += w;
                }
            ao = sum / max(wsum, 1e-4);
            // temporal accumulation along the motion vector, rejected on depth mismatch
            vec4 aux = texture(sceneAux, vUv);
            vec2 prevUv = vUv - aux.rg;
            if (pc.params.y > 0.5 && all(greaterThanEqual(prevUv, vec2(0.0))) && all(lessThanEqual(prevUv, vec2(1.0)))) {
                vec4 h = texture(aoHistory, prevUv);
                float ok = step(abs(h.g - lin), max(lin * 0.08, 2e-4));
                ao = mix(ao, h.r, 0.88 * ok);
            }
            if (depth >= 1.0) ao = 1.0;
            color *= mix(1.0, ao, 0.7);
        }
        color += pc.lightAdd.rgb;
        color *= pc.params2.x;
        if (int(pc.params2.y) == 3) color = vec3(ao);
    }
    outHistory = vec4(ao, lin, 0.0, 1.0);

    vec2 ouv = (vUv - pc.overlayRect.xy) / pc.overlayRect.zw;
    if (all(greaterThanEqual(ouv, vec2(0.0))) && all(lessThanEqual(ouv, vec2(1.0)))) {
        vec4 o = texture(overlay, ouv);
        color = mix(color, o.rgb, o.a);
    } else if (pc.lightAdd.a < 0.5)
        color = vec3(0.0);
    outColor = vec4(color, 1.0);
}
