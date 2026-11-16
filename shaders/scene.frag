#version 460
// Forward shading for every scene primitive. Without RT this is the classic LithTech look (texture x lightmap,
// stepped model lighting, unshadowed dynamic lights). With -DRT the same shader adds ray-queried shadows,
// reflections and ambient occlusion against the frame's TLAS.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#ifdef RT
#extension GL_EXT_ray_query : require
layout(set = 0, binding = 9) uniform accelerationStructureEXT tlas;
#endif

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec2 vLm;
layout(location = 4) in vec4 vColor;
layout(location = 5) in vec4 vClip;
layout(location = 6) in vec4 vPrevClip;
layout(location = 7) flat in uint vInstance;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outAux;       // rg = screen-space motion (uv units), b = ambient occlusion, a = packed normal
layout(location = 3) out vec4 outIndirect;  // rgb = noisy bounce light of composed pixels (filtered and accumulated by the resolve)
layout(location = 2) out vec4 outLight;     // rgb = light to multiply onto outColor (albedo) in the resolve, a = 1; a = 0: outColor is final

vec4 sampleTex(uint tex, vec2 texelUv) {
    if (tex == 0u) return vec4(1.0);
    vec2 size = vec2(textureSize(textures[nonuniformEXT(tex)], 0));
    return texture(textures[nonuniformEXT(tex)], texelUv / size);
}
vec4 sampleTexLod(uint tex, vec2 texelUv, float lod) {
    if (tex == 0u) return vec4(1.0);
    vec2 size = vec2(textureSize(textures[nonuniformEXT(tex)], 0));
    return textureLod(textures[nonuniformEXT(tex)], texelUv / size, lod);
}

// Generated normal map (host: from the texture's own luminance) applied in a cotangent frame built from screen-space
// derivatives, so no vertex tangents are needed. Not game data: an invented relief for the ray-traced lighting.
vec3 bumpNormal(vec3 n, uint tex, vec2 texelUv, float strength) {
    if (tex == 0u || tex >= TEX_NORMAL_BASE - 1u || strength <= 0.0) return n;
    vec2 uv = texelUv / vec2(textureSize(textures[nonuniformEXT(tex)], 0));
    vec3 tn = texture(textures[nonuniformEXT(tex + TEX_NORMAL_BASE)], uv).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(vWorld), dp2 = dFdy(vWorld);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x, B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-20) return n;
    float inv = inversesqrt(m);
    return normalize((T * tn.x + B * tn.y) * inv * strength + n * max(tn.z, 0.05));
}

// LithTech's model ramp: 16 steps from ambient to ambient + directional, half-Lambert (d3d.ren 0x10e7c).
vec3 modelLight(Instance I, vec3 n) {
    float s = clamp(floor(dot(n, frame.modelLightDir.xyz) * 7.94 + 8.0), 0.0, 15.0) / 16.0;
    return I.color.rgb + I.light.rgb * s;
}

float interleavedNoise(vec2 p, float frameIndex) {
    p += 5.588238 * mod(frameIndex, 64.0);
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#ifdef RT
float traceVisibility(vec3 origin, vec3 dir, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0x07, origin, 0.25, dir, tmax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// TLAS masks: 1 world, 2 dynamic models, 4 solid world models, 8 alpha-masked world models (non-opaque geometry:
// every candidate is alpha-tested against its texture here).
float traceMasked(vec3 origin, vec3 dir, float tmax, uint mask) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, 0.25, dir, tmax);
    while (rayQueryProceedEXT(rq)) {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        Instance C = instances[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false))];
        uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
        vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, false);
        vec2 uv = vertices[indices[C.firstIndex + prim * 3u]].uv * (1.0 - bc.x - bc.y) + vertices[indices[C.firstIndex + prim * 3u + 1u]].uv * bc.x +
                  vertices[indices[C.firstIndex + prim * 3u + 2u]].uv * bc.y;
        uint tex = triSurface(C.triBase, prim).tex;
        float a = C.color.a;
        if ((texInfo[tex].flags & T_ALPHA) != 0u) a *= sampleTexLod(tex, uv, 0.0).a;
        if (a >= 0.5) rayQueryConfirmIntersectionEXT(rq);
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// ---- the level's static lamps (host/lamps.cpp). Terms are in lightmap units: the bake was
// ambient + colour * BrightScale * (1 - d/r) * cone, one-sided.
vec3 lampAt(Lamp l, vec3 p, out vec3 toLamp, out float dist) {
    toLamp = l.posRadius.xyz - p;
    dist = length(toLamp);
    if (dist >= l.posRadius.w) return vec3(0.0);
    float f = 1.0 - dist / l.posRadius.w;
    if (l.spot.w > -1.5) {
        float c = dist > 0.001 ? dot(l.spot.xyz, -toLamp) / dist : 1.0;
        f *= max((c - l.spot.w) / (1.0 - l.spot.w), 0.0);
    }
    return l.color.rgb * f;
}
bool lampCell(vec3 p, out uint begin, out uint end) {
    ivec3 c = ivec3(floor((p - frame.lampGridOrigin.xyz) / frame.lampGridOrigin.w));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, frame.lampGridDims))) return false;
    uint i = uint((c.z * frame.lampGridDims.y + c.y) * frame.lampGridDims.x + c.x);
    begin = lampData[i]; end = lampData[i + 1u];
    return end > begin;
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// Baked light with the lamps taken out where something that was not in the bake (models, doors) now blocks them.
// Only light the bake shows arrived is removed, and never below the level's ambient.
vec3 worldLampShadows(vec3 baked, vec3 p, vec3 n, vec3 origin, uint mask) {
    uint b, e;
    if (!lampCell(p, b, e)) return baked;
    vec3 direct = max(baked - frame.ambient.rgb, vec3(0.0));
    float received = luma(direct);
    if (received < 0.01) return baked;
    vec3 loss = vec3(0.0);
    int rays = 0;
    for (uint k = b; k < e && rays < 12; k++) {
        Lamp l = lamps[lampData[k]];
        if ((uint(l.color.w) & 1u) == 0u) continue;
        vec3 to; float d;
        vec3 c = lampAt(l, p, to, d);
        float expected = luma(c);
        if (expected < 0.02 || dot(n, to) <= 0.0) continue;
        rays++;
        if (traceMasked(origin, to / d, max(d - 2.0, 0.0), mask) < 0.5) loss += c * smoothstep(0.25, 0.75, received / expected);
    }
    return baked - min(loss, direct);
}

// Pre-lit geometry (alpha-masked walls, glass): the vertex colours hold the bake's unshadowed result. The share of the
// lamps reaching the point that is blocked now is taken out of it, so fences sit in shadows like everything else.
vec3 prelitLampShadows(vec3 prelit, vec3 p, vec3 origin) {
    uint b, e;
    if (!lampCell(p, b, e)) return prelit;
    vec3 sum = vec3(0.0), lit = vec3(0.0);
    int rays = 0;
    for (uint k = b; k < e && rays < 8; k++) {
        Lamp l = lamps[lampData[k]];
        if ((uint(l.color.w) & 1u) == 0u) continue;
        vec3 to; float d;
        vec3 c = lampAt(l, p, to, d);
        if (luma(c) < 0.02) continue;
        rays++;
        sum += c;
        lit += c * traceMasked(origin, to / d, max(d - 2.0, 0.0), 0x07u);
    }
    if (luma(sum) < 0.01) return prelit;
    vec3 direct = max(prelit - frame.ambient.rgb, vec3(0.0));
    return prelit - direct * (1.0 - clamp(luma(lit) / luma(sum), 0.0, 1.0));
}

// Model lighting from the lamps: each lamp that lit models in the bake, shadowed by everything, replaces its share of
// the light grid's directional term; what the lamps do not explain stays as the classic stepped term.
vec3 modelLampLight(Instance I, vec3 p, vec3 n, vec3 origin, float classicStep) {
    uint b, e;
    if (!lampCell(p, b, e)) return I.color.rgb + I.light.rgb * classicStep;
    vec3 lit = vec3(0.0), unshadowed = vec3(0.0);
    int rays = 0;
    for (uint k = b; k < e; k++) {
        Lamp l = lamps[lampData[k]];
        if ((uint(l.color.w) & 2u) == 0u) continue;
        vec3 to; float d;
        vec3 c = lampAt(l, p, to, d);
        if (luma(c) < 0.02) continue;
        unshadowed += c;
        float ndl = clamp(dot(n, to / d) * 0.55 + 0.45, 0.0, 1.0);
        if (ndl <= 0.0) continue;
        float vis = 1.0;
        if (rays < 10) { vis = traceMasked(origin, to / d, max(d - 2.0, 0.0), 0xffu); rays++; }   // fences shade models too
        lit += c * ndl * vis;
    }
    // keep the frame's energy: lamps may sum to more than the grid recorded (the bake clamps)
    float scale = min(1.0, luma(I.light.rgb) / max(luma(unshadowed), 0.001));
    vec3 residual = max(I.light.rgb - unshadowed, vec3(0.0));
    return I.color.rgb + residual * classicStep + lit * scale;
}

// ---- full ray-traced lighting (lr_RTGI): no lightmaps on the receiving surface
uint rngState;
float rnd() {       // PCG
    rngState = rngState * 747796405u + 2891336453u;
    uint w = ((rngState >> ((rngState >> 28u) + 4u)) ^ rngState) * 277803737u;
    return float((w >> 22u) ^ w) * (1.0 / 4294967296.0);
}
vec3 rndSphere() {
    float z = rnd() * 2.0 - 1.0, a = rnd() * 6.2831853, r = sqrt(max(1.0 - z * z, 0.0));
    return vec3(r * cos(a), r * sin(a), z);
}
vec3 cosineDir(vec3 n, vec2 u);

// Direct light of the level's lamps at p: every lamp in reach, Lambert, one shadow ray each towards a random point
// of a small sphere around the lamp (soft edges once accumulated).
vec3 lampDirect(vec3 p, vec3 n, vec3 origin, uint wantFlags, int maxRays, vec3 view, float shininess, out vec3 spec) {
    spec = vec3(0.0);
    uint b, e;
    if (!lampCell(p, b, e)) return vec3(0.0);
    vec3 sum = vec3(0.0);
    int rays = 0;
    for (uint k = b; k < e; k++) {
        Lamp l = lamps[lampData[k]];
        if ((uint(l.color.w) & wantFlags) == 0u) continue;
        vec3 to; float d;
        vec3 c = lampAt(l, p, to, d);
        float ndl = dot(n, to) / max(d, 0.001);
        if (ndl <= 0.0) continue;
        // The level was lit for a bake WITHOUT a cosine term (a lamp lights every surface facing it equally). Pure
        // Lambert makes walls much darker than the designers saw; half-way keeps their balance and still reads as form.
        ndl = 0.5 + 0.5 * ndl;
        if (luma(c) * ndl < 0.01) continue;
        float vis = 1.0;
        if (rays < maxRays) {
            vis = traceMasked(origin, to / d, max(d - 2.0, 0.0), 0xffu);
            rays++;
        }
        sum += c * ndl * vis;
        if (vis > 0.0) spec += c * vis * pow(max(dot(n, normalize(to / d + view)), 0.0), shininess);
    }
    spec *= frame.gi.z;
    return sum * frame.gi.z;
}

// What an indirect ray brings back: the hit surface's albedo lit by ONE randomly chosen lamp (with its own shadow
// ray) plus the level ambient; emissive (fullbright) texels and the sky are light sources of their own.
vec3 bounceRadiance(vec3 origin, vec3 dir) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0x07, origin, 0.25, dir, 4000.0);
    while (rayQueryProceedEXT(rq)) {}
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return frame.skyColor.rgb;
    uint ci = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    float t = rayQueryGetIntersectionTEXT(rq, true);
    vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    Instance C = instances[ci];
    Vertex va = vertices[indices[C.firstIndex + prim * 3u]];
    Vertex vb = vertices[indices[C.firstIndex + prim * 3u + 1u]];
    Vertex vc = vertices[indices[C.firstIndex + prim * 3u + 2u]];
    vec2 uv = va.uv * w.x + vb.uv * w.y + vc.uv * w.z;
    vec3 hn = normalize(xformDir(C.r0, C.r1, C.r2, va.normal * w.x + vb.normal * w.y + vc.normal * w.z));
    if (dot(hn, dir) > 0.0) hn = -hn;
    vec3 hp = origin + dir * t;
    uint tex; vec3 base = vec3(0.0); bool bright = false;
    if ((C.flags & I_WORLD) != 0u) {
        Surface s = triSurface(C.triBase, prim);
        tex = s.tex; bright = (s.flags & SURF_BRIGHT) != 0u;
    } else {
        Instance M = C;
        if ((C.flags & I_CONTAINER) != 0u) M = instances[triTable[C.triBase + prim]];
        tex = (M.flags & I_NOTEXTURE) != 0u ? 0u : M.tex; base = M.color.rgb;
    }
    vec4 tx = sampleTexLod(tex, uv, clamp(log2(max(t, 1.0) / 64.0), 0.0, 3.0));
    float emissive = bright ? 1.0 : ((texInfo[tex].flags & T_FULLBRIGHT) != 0u ? tx.a : 0.0);
    vec3 irr = frame.ambient.rgb + base;
    uint b, e;
    if (lampCell(hp, b, e)) {
        uint count = e - b;
        Lamp l = lamps[lampData[b + min(uint(rnd() * float(count)), count - 1u)]];
        vec3 to; float d;
        vec3 c = lampAt(l, hp, to, d);
        float ndl = dot(hn, to) / max(d, 0.001);
        if (ndl > 0.0 && luma(c) > 0.01)
            irr += min(c * (0.5 + 0.5 * ndl) * float(count), vec3(3.0)) * traceMasked(hp + hn * 0.75, to / d, max(d - 2.0, 0.0), 0x07u) * frame.gi.z;
    }
    return tx.rgb * mix(irr * frame.lightScale.rgb * frame.lightScale.a, vec3(1.5), emissive);
}

// Classic shading of whatever a ray hit (no recursion): albedo x baked light.
vec3 shadeHit(rayQueryEXT rq, vec3 origin, vec3 dir) {
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return frame.skyColor.rgb;
    uint ci = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    vec2 bc = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    float t = rayQueryGetIntersectionTEXT(rq, true);
    vec3 w = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    Instance C = instances[ci];
    Vertex a = vertices[indices[C.firstIndex + prim * 3u]];
    Vertex b = vertices[indices[C.firstIndex + prim * 3u + 1u]];
    Vertex c = vertices[indices[C.firstIndex + prim * 3u + 2u]];
    vec2 uv = a.uv * w.x + b.uv * w.y + c.uv * w.z;
    vec2 lm = a.lmuv * w.x + b.lmuv * w.y + c.lmuv * w.z;
    vec4 vc = unpackUnorm4x8(a.color) * w.x + unpackUnorm4x8(b.color) * w.y + unpackUnorm4x8(c.color) * w.z;
    vec3 n = normalize(a.normal * w.x + b.normal * w.y + c.normal * w.z);
    float lod = clamp(log2(max(t, 1.0) / 96.0), 0.0, 3.0);
    vec3 color;
    // with full ray-traced lighting the reflected surface is lit by the lamps too (unshadowed here), not by its lightmap
    bool lampLit = frame.gi.x > 0.5;
    vec3 hitLamps = vec3(0.0);
    if (lampLit) {
        vec3 hp = origin + dir * t;
        vec3 hn = normalize(xformDir(C.r0, C.r1, C.r2, n));
        if (dot(hn, dir) > 0.0) hn = -hn;
        hitLamps = frame.ambient.rgb * 0.5;
        uint lb, le;
        if (lampCell(hp, lb, le))
            for (uint k = lb; k < le && k < lb + 16u; k++) {
                vec3 to; float d;
                vec3 c = lampAt(lamps[lampData[k]], hp, to, d);
                float hndl = dot(hn, to) / max(d, 0.001);
                if (hndl > 0.0) hitLamps += c * (0.5 + 0.5 * hndl) * frame.gi.z;
            }
        float hl = luma(hitLamps);
        if (hl > 0.5) hitLamps *= (0.5 + (hl - 0.5) / (1.0 + (hl - 0.5) * 1.1)) / hl;
    }
    if ((C.flags & I_WORLD) != 0u) {
        Surface s = triSurface(C.triBase, prim);
        vec4 tx = sampleTexLod(s.tex, uv, lod);
        vec3 light = (s.flags & SURF_BRIGHT) != 0u ? vec3(1.0) : (lampLit && (C.flags & I_PRELIT) == 0u) ? hitLamps
                   : ((s.flags & SURF_LIGHTMAP) != 0u ? textureLod(textures[TEX_LIGHTMAP], lm, 0.0).rgb : vc.rgb);
        light *= frame.lightScale.rgb * frame.lightScale.a;
        color = tx.rgb * light;
        if ((texInfo[s.tex].flags & T_FULLBRIGHT) != 0u) color = mix(color, tx.rgb, tx.a);
    } else {
        Instance M = C;
        if ((C.flags & I_CONTAINER) != 0u) M = instances[triTable[C.triBase + prim]];
        vec4 tx = (M.flags & I_NOTEXTURE) != 0u ? vec4(1.0) : sampleTexLod(M.tex, uv, lod);
        vec3 light = (M.flags & I_MODEL) != 0u ? (lampLit ? M.color.rgb + hitLamps : modelLight(M, n)) : M.color.rgb * vc.rgb;
        color = tx.rgb * light;
    }
    if (frame.fogColor.a > 0.5) color = mix(color, frame.fogColor.rgb, clamp((t - frame.fogParams.x) / max(frame.fogParams.y - frame.fogParams.x, 1.0), 0.0, 1.0));
    return color;
}

vec3 traceRadiance(vec3 origin, vec3 dir, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0x07, origin, 0.25, dir, tmax);
    while (rayQueryProceedEXT(rq)) {}
    return shadeHit(rq, origin, dir);
}

vec3 cosineDir(vec3 n, vec2 u) {
    float phi = 6.2831853 * u.x, r = sqrt(u.y);
    vec3 t = normalize(abs(n.y) < 0.99 ? cross(n, vec3(0, 1, 0)) : cross(n, vec3(1, 0, 0)));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u.y, 0.0)));
}
#endif

void main() {
    Instance I = instances[vInstance];
    uint tex = I.tex, sflags = 0u;
    bool world = (I.flags & I_WORLD) != 0u;
    if (world) {
        Surface s = triSurface(I.triBase, uint(gl_PrimitiveID));
        tex = s.tex; sflags = s.flags;
    }
    vec4 tx = (I.flags & I_NOTEXTURE) != 0u ? vec4(1.0) : sampleTex(tex, vUv);
    uint tflags = texInfo[tex].flags;
    float gloss = texInfo[tex].gloss;

    vec3 n = normalize(vNormal);
    // sky objects live in their own space and are viewed (culled, fogged) from the sky camera
    vec3 toCam = ((I.flags & I_SKY) != 0u ? frame.skyEye.xyz : frame.camPos.xyz) - vWorld;
    float dist = length(toCam);
    vec3 v = toCam / max(dist, 0.001);
    if (dot(n, v) < 0.0 && (I.flags & (I_MODEL | I_WORLD)) == 0u) n = -n;
    // world polygons are one-sided (d3d.ren culls by the polygon's plane); the camera may sit inside a brush
    if (world) {
        vec3 eye = (I.flags & I_SKY) != 0u ? frame.skyEye.xyz : frame.camPos.xyz;
        bool back = dot(n, eye - vWorld) < 0.0;
        if (back && (I.flags & I_TWOSIDED) == 0u) discard;
        if (back) n = -n;
    }
    vec3 geoN = n;
    // shading normal with the generated relief (lit world and model surfaces only)
    vec3 nb = geoN;
    if (frame.mat.x > 0.0 && (I.flags & (I_UNLIT | I_SKY | I_PRELIT)) == 0u) nb = bumpNormal(geoN, tex, vUv, frame.mat.x);

    float alpha = I.color.a;
    // d3d.ren's translucent states: alpha = texture alpha x object alpha. On a 1555 (fullbright) texture the alpha bit is
    // set on the fullbright palette entries only, so in a blended pass it is the texel's opacity (the sky's moon).
    bool alphaOpacity = (tflags & T_ALPHA) != 0u || ((tflags & T_FULLBRIGHT) != 0u && (I.flags & I_BLEND) != 0u);
    if (alphaOpacity) alpha *= tx.a;
    if ((I.flags & I_ALPHATEST) != 0u && tx.a < 0.5) discard;

    float rtOn = 0.0;
#ifdef RT
    rtOn = frame.rt.x;
#endif

    // ---- baked / classic lighting
    vec3 light;
    float bakedOcc = 1.0;
    if (world) {
        bool prelit = (I.flags & I_PRELIT) != 0u;
        light = (!prelit && (sflags & SURF_BRIGHT) != 0u) ? vec3(1.0)
              : ((!prelit && (sflags & SURF_LIGHTMAP) != 0u) ? texture(textures[TEX_LIGHTMAP], vLm).rgb : vColor.rgb);
#ifdef RT
        if (frame.ambient.a > 0.5 && prelit && (I.flags & I_SKY) == 0u)
            light = prelitLampShadows(light, vWorld, vWorld + geoN * 1.0);
        else if (frame.ambient.a > 0.5 && (sflags & (SURF_LIGHTMAP | SURF_BRIGHT)) == SURF_LIGHTMAP)
            light = worldLampShadows(light, vWorld, geoN, vWorld + geoN * 0.75, (I.flags & I_SKY) != 0u ? 0u : (vInstance == 1u ? 0x0eu : 0x0au));
#endif
        light *= frame.lightScale.rgb * frame.lightScale.a;
    } else if ((I.flags & I_MODEL) != 0u) {
        light = modelLight(I, n);
#ifdef RT
        if (frame.ambient.a > 0.5 && (I.flags & (I_CLOSE | I_SKY)) == 0u) {
            float s = clamp(floor(dot(n, frame.modelLightDir.xyz) * 7.94 + 8.0), 0.0, 15.0) / 16.0;
            light = modelLampLight(I, vWorld, n, vWorld + n * 1.5, s);
        }
#endif
    } else {
        light = I.color.rgb * vColor.rgb;
        alpha *= vColor.a;
    }

    // ---- dynamic lights (signed colours). Classic: no N.L and no occlusion.
    bool lit = (I.flags & I_UNLIT) == 0u && (sflags & SURF_BRIGHT) == 0u;
    vec3 dynLight = vec3(0.0);
    float ao = 1.0;
    vec3 origin = vWorld + geoN * 0.75;
    if (lit) {
        for (uint i = 0u; i < frame.nLights; i++) {
            vec3 d = lights[i].posRadius.xyz - vWorld;
            float r = lights[i].posRadius.w, d2 = dot(d, d);
            if (d2 >= r * r) continue;
            float att = 1.0 - d2 / (r * r);
            float len = sqrt(d2);
            float term = att;
#ifdef RT
            if (rtOn > 0.5) {
                float ndl = clamp(dot(nb, d / max(len, 0.001)) * 0.75 + 0.25, 0.0, 1.0);
                term = att * ndl * 1.6;
                if (frame.rt.y > 0.5 && term > 0.004 && (I.flags & I_CLOSE) == 0u)
                    term *= traceMasked(origin, d / max(len, 0.001), max(len - 1.0, 0.0), (I.flags & I_PRELIT) != 0u ? 0x07u : 0xffu);
            }
#endif
            dynLight += lights[i].color.rgb * term;
        }
        light = max(light + dynLight, vec3(0.0));
    }

    // ---- full ray-traced lighting: lamps and one bounce replace the baked light of opaque world and model surfaces
    bool composed = false;
    vec3 bounceOut = vec3(0.0), specOut = vec3(0.0);
#ifdef RT
    {
        bool surfWorld = world && (I.flags & (I_PRELIT | I_SKY | I_BLEND)) == 0u && (sflags & SURF_BRIGHT) == 0u;
        bool surfModel = (I.flags & I_MODEL) != 0u && (I.flags & (I_CLOSE | I_SKY)) == 0u && I.color.a >= 0.999;
        if (rtOn > 0.5 && frame.gi.x > 0.5 && frame.lampCount > 0u && (surfWorld || surfModel)) {
            composed = true;
            rngState = (uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + uint(frame.misc.x) * 26699u) | 1u;
            rnd();
            vec3 o = vWorld + geoN * (surfModel ? 1.5 : 0.75);
            vec3 spec;
            vec3 direct = lampDirect(vWorld, nb, o, surfModel ? 3u : 1u, 8, v, max(texInfo[tex].shininess, 4.0), spec);
            specOut = spec * texInfo[tex].spec * frame.mat.y * frame.lightScale.rgb;
            vec3 bounce = bounceRadiance(o, cosineDir(geoN, vec2(rnd(), rnd()))) * frame.gi.y;
            vec3 scale = frame.lightScale.rgb * frame.lightScale.a;
            // keep only the dynamic-light part of the classic term: the baked part is what the rays replace
            // direct light stays sharp (never filtered); the bounce goes to its own buffer for the resolve to denoise
            light = (surfModel ? I.color.rgb : frame.ambient.rgb * 0.5 * scale) + direct * scale + dynLight;
            bounceOut = bounce;
        }
    }
#endif
    vec3 color = tx.rgb * light + specOut;
    // the texture's fullbright layer; in the composed path the resolve applies it, so it bypasses the light filter
    float fbMask = ((tflags & T_FULLBRIGHT) != 0u && !alphaOpacity) ? tx.a : 0.0;
    if (!composed) color = mix(color, tx.rgb, fbMask);

#ifdef RT
    bool water = (I.flags & I_WATER) != 0u;
    // d3d.ren's chrome: the environment map is drawn first and the skin blended over it with alpha = skin texture alpha
    // x object alpha, so (1 - that) is the game's own per-texel reflectivity on FLAG_ENVIRONMENTMAP models
    float chrome = 0.0;
    if ((I.flags & I_CHROME) != 0u) chrome = clamp(1.0 - ((tflags & T_ALPHA) != 0u ? tx.a : 1.0) * I.light.a, 0.0, 1.0);
    bool close = (I.flags & I_CLOSE) != 0u;
    if (rtOn > 0.5 && (I.flags & I_SKY) == 0u && (!close || chrome > 0.0) && ((I.flags & I_UNLIT) == 0u || water)) {
        float noise = interleavedNoise(gl_FragCoord.xy, frame.misc.x);
        // ---- ambient occlusion: short rays over the hemisphere, modulating the baked light
        if (frame.rt.w > 0.5 && !water && !composed && !close) {
            const int N = 2;
            float occ = 0.0;
            for (int k = 0; k < N; k++) {
                vec2 u = vec2(fract(noise + float(k) * 0.618034), fract(noise * 7.13 + float(k) * 0.4142 + 0.37));
                occ += traceVisibility(origin, cosineDir(geoN, u), 48.0);
            }
            ao = occ / float(N);
        }
        // ---- reflections: one mirror ray, weighted by Fresnel and the material's gloss
        float ndv = clamp(dot(geoN, v), 0.0, 1.0);
        // chrome models: the original's fixed skin / environment mix; water: stronger at grazing angles; other
        // surfaces: a keyword-derived gloss with a Fresnel curve (not from the game's data)
        float fres = max(close ? 0.0 : gloss * (0.25 + 0.75 * pow(1.0 - ndv, 4.0)),
                         water ? I.light.a * (0.4 + 0.6 * pow(1.0 - ndv, 3.0)) : chrome);
        if (frame.rt.z > 0.5 && fres > 0.01) {
            vec3 rdir = reflect(-v, normalize(mix(geoN, nb, 0.6)));
            if (dot(rdir, geoN) < 0.02) rdir = reflect(-v, geoN);
            vec3 rcol = traceRadiance(origin, rdir, 6000.0);
            color = mix(color, rcol, clamp(fres * frame.rt.z, 0.0, 1.0));
        }
    }
#endif

    // ---- fog (by eye distance)
    if (frame.fogColor.a > 0.5 && (I.flags & I_CLOSE) == 0u && !composed) {
        vec2 range = (I.flags & I_SKY) != 0u ? frame.fogParams.zw : frame.fogParams.xy;
        color = mix(color, frame.fogColor.rgb, clamp((dist - range.x) / max(range.y - range.x, 1.0), 0.0, 1.0));
    }

    int dbg = int(frame.misc.y);
    if (dbg == 1) color = n * 0.5 + 0.5;
    else if (dbg == 2) color = light;
    else if (dbg == 3) color = vec3(ao);
    else if (dbg == 4) color = tx.rgb;
    else if (dbg == 5) color = vec3(fract(vWorld / 256.0));
    else if (dbg == 8 && world) {
        // where a world surface's baked light comes from: r = has a lightmap, g = baked vertex colour, b = lightmap value
        color = vec3((sflags & SURF_LIGHTMAP) != 0u ? 1.0 : 0.0, dot(vColor.rgb, vec3(0.333)),
                     (sflags & SURF_LIGHTMAP) != 0u ? dot(texture(textures[TEX_LIGHTMAP], vLm).rgb, vec3(0.333)) : 0.0);
    } else if (dbg == 9 && world) {
        // r = surface flag 0x40, g = surface flag 0x1000, b = world model (not the main world)
        color = vec3((sflags & 0x40u) != 0u ? 1.0 : 0.0, (sflags & 0x1000u) != 0u ? 1.0 : 0.0, vInstance != 1u ? 1.0 : 0.15);
    }
    else if (dbg == 6 || dbg == 7) {
        // 6: one colour per instance, 7: one colour per world surface (red channel 1 = world model, 0 = main world)
        uint id = dbg == 6 ? vInstance : (world ? (triTable[I.triBase + uint(gl_PrimitiveID)] & 0x7fffffffu) : 0u);
        uint hsh = id * 2654435761u;
        color = vec3(float((hsh >> 8) & 255u), float((hsh >> 16) & 255u), float((hsh >> 24) & 255u)) / 255.0 * (0.4 + 0.6 * clamp(dot(n, v), 0.0, 1.0));
        if (dbg == 7) color.r = (world && vInstance != 1u) ? 1.0 : 0.0;
    }

    vec2 cur = vClip.xy / vClip.w, prev = vPrevClip.xy / vPrevClip.w;
    // normal, octahedral, 12 bits per axis in one float (the resolve's edge-stopping filter reads it)
    vec2 oct = geoN.xy / (abs(geoN.x) + abs(geoN.y) + abs(geoN.z));
    if (geoN.z < 0.0) oct = (1.0 - abs(oct.yx)) * vec2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
    vec2 q = floor(clamp(oct * 0.5 + 0.5, 0.0, 1.0) * 4095.0 + 0.5);
    outAux = vec4((cur - prev) * 0.5, ao, q.x + q.y * 4096.0);
    if (composed && dbg != 0 && dbg != 2) composed = false;
    if (composed) {
        // the resolve multiplies the filtered light back onto the albedo: store colour / albedo as the light
        vec3 albedo = dbg == 2 ? vec3(1.0) : max(tx.rgb, vec3(0.04));
        outColor = vec4(albedo, dbg == 2 ? 0.0 : fbMask);
        outLight = vec4((dbg == 2 ? light : color) / (dbg == 2 ? vec3(1.0) : albedo), 1.0);
        outIndirect = vec4(bounceOut, 1.0);
    } else {
        outColor = vec4(color, alpha);
        outLight = vec4(0.0);
        outIndirect = vec4(0.0);
    }
}
