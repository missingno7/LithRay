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
layout(location = 1) out vec4 outAux;       // rg = screen-space motion (uv units), b = ambient occlusion, a = 1

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
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xff, origin, 0.25, dir, tmax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
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
    if ((C.flags & I_WORLD) != 0u) {
        Surface s = surfaces[triTable[C.triBase + prim]];
        vec4 tx = sampleTexLod(s.tex, uv, lod);
        vec3 light = (s.flags & SURF_LIGHTMAP) != 0u ? textureLod(textures[TEX_LIGHTMAP], lm, 0.0).rgb
                   : ((s.flags & SURF_BRIGHT) != 0u ? vec3(1.0) : vc.rgb);
        light *= frame.lightScale.rgb * frame.lightScale.a;
        color = tx.rgb * light;
        if ((texInfo[s.tex].flags & T_FULLBRIGHT) != 0u) color = mix(color, tx.rgb, tx.a);
    } else {
        Instance M = C;
        if ((C.flags & I_CONTAINER) != 0u) M = instances[triTable[C.triBase + prim]];
        vec4 tx = (M.flags & I_NOTEXTURE) != 0u ? vec4(1.0) : sampleTexLod(M.tex, uv, lod);
        vec3 light = (M.flags & I_MODEL) != 0u ? modelLight(M, n) : M.color.rgb * vc.rgb;
        color = tx.rgb * light;
    }
    if (frame.fogColor.a > 0.5) color = mix(color, frame.fogColor.rgb, clamp((t - frame.fogParams.x) / max(frame.fogParams.y - frame.fogParams.x, 1.0), 0.0, 1.0));
    return color;
}

vec3 traceRadiance(vec3 origin, vec3 dir, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xff, origin, 0.25, dir, tmax);
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
        Surface s = surfaces[triTable[I.triBase + uint(gl_PrimitiveID)]];
        tex = s.tex; sflags = s.flags;
    }
    vec4 tx = (I.flags & I_NOTEXTURE) != 0u ? vec4(1.0) : sampleTex(tex, vUv);
    uint tflags = texInfo[tex].flags;
    float gloss = texInfo[tex].gloss;

    vec3 n = normalize(vNormal);
    vec3 toCam = frame.camPos.xyz - vWorld;
    float dist = length(toCam);
    vec3 v = toCam / max(dist, 0.001);
    if (dot(n, v) < 0.0 && (I.flags & (I_MODEL | I_WORLD)) == 0u) n = -n;
    vec3 geoN = n;
    if (world && dot(n, v) < 0.0) geoN = -n;

    float alpha = I.color.a;
    if ((tflags & T_ALPHA) != 0u) alpha *= tx.a;
    if ((I.flags & I_ALPHATEST) != 0u && tx.a < 0.5) discard;

    float rtOn = 0.0;
#ifdef RT
    rtOn = frame.rt.x;
#endif

    // ---- baked / classic lighting
    vec3 light;
    float bakedOcc = 1.0;
    if (world) {
        light = (sflags & SURF_LIGHTMAP) != 0u ? texture(textures[TEX_LIGHTMAP], vLm).rgb
              : ((sflags & SURF_BRIGHT) != 0u ? vec3(1.0) : vColor.rgb);
        light *= frame.lightScale.rgb * frame.lightScale.a;
    } else if ((I.flags & I_MODEL) != 0u) {
        light = modelLight(I, n);
    } else {
        light = I.color.rgb * vColor.rgb;
        alpha *= vColor.a;
    }

    // ---- dynamic lights (signed colours). Classic: no N.L and no occlusion.
    bool lit = (I.flags & I_UNLIT) == 0u && (sflags & SURF_BRIGHT) == 0u;
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
                float ndl = clamp(dot(geoN, d / max(len, 0.001)) * 0.75 + 0.25, 0.0, 1.0);
                term = att * ndl * 1.6;
                if (frame.rt.y > 0.5 && term > 0.004 && (I.flags & I_CLOSE) == 0u)
                    term *= traceVisibility(origin, d / max(len, 0.001), max(len - 1.0, 0.0));
            }
#endif
            light += lights[i].color.rgb * term;
        }
        light = max(light, vec3(0.0));
    }

    vec3 color = tx.rgb * light;
    if ((tflags & T_FULLBRIGHT) != 0u) color = mix(color, tx.rgb, tx.a);

#ifdef RT
    bool water = (I.flags & I_WATER) != 0u;
    if (rtOn > 0.5 && (I.flags & (I_SKY | I_CLOSE)) == 0u && ((I.flags & I_UNLIT) == 0u || water)) {
        float noise = interleavedNoise(gl_FragCoord.xy, frame.misc.x);
        // ---- ambient occlusion: short rays over the hemisphere, modulating the baked light
        if (frame.rt.w > 0.5 && !water) {
            const int N = 2;
            float occ = 0.0;
            for (int k = 0; k < N; k++) {
                vec2 u = vec2(fract(noise + float(k) * 0.618034), fract(noise * 7.13 + float(k) * 0.4142 + 0.37));
                occ += traceVisibility(origin, cosineDir(geoN, u), 48.0);
            }
            ao = occ / float(N);
        }
        // ---- reflections: one mirror ray, weighted by Fresnel and the material's gloss
        float refl = max(gloss, I.light.a);
        if (frame.rt.z > 0.5 && refl > 0.01) {
            float ndv = clamp(dot(geoN, v), 0.0, 1.0);
            float fres = refl * (0.25 + 0.75 * pow(1.0 - ndv, 4.0));
            vec3 rdir = reflect(-v, geoN);
            vec3 rcol = traceRadiance(origin, rdir, 6000.0);
            color = mix(color, rcol, clamp(fres * frame.rt.z, 0.0, 1.0));
        }
    }
#endif

    // ---- fog (by eye distance)
    if (frame.fogColor.a > 0.5 && (I.flags & I_CLOSE) == 0u) {
        vec2 range = (I.flags & I_SKY) != 0u ? frame.fogParams.zw : frame.fogParams.xy;
        color = mix(color, frame.fogColor.rgb, clamp((dist - range.x) / max(range.y - range.x, 1.0), 0.0, 1.0));
    }

    int dbg = int(frame.misc.y);
    if (dbg == 1) color = n * 0.5 + 0.5;
    else if (dbg == 2) color = light;
    else if (dbg == 3) color = vec3(ao);
    else if (dbg == 4) color = tx.rgb;
    else if (dbg == 5) color = vec3(fract(vWorld / 256.0));

    outColor = vec4(color, alpha);
    vec2 cur = vClip.xy / vClip.w, prev = vPrevClip.xy / vPrevClip.w;
    outAux = vec4((cur - prev) * 0.5, ao, 1.0);
}
