// Shared GPU scene description. The same buffers feed rasterisation (vertex attributes, per-draw instance) and
// ray queries (hit shading looks the triangle up through the TLAS instance's custom index). Mirrors host/scene_gpu.h.
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_TEXTURES 4096
#define TEX_LIGHTMAP 4095u          // reserved slot: the lightmap atlas of the current world

// Instance.flags
#define I_WORLD      0x1u           // world BSP geometry: per-triangle surface via triTable, lightmapped
#define I_CONTAINER  0x2u           // RT only: triTable gives the real instance per triangle (dynamic BLAS)
#define I_MODEL      0x4u           // ambient + directional model lighting
#define I_UNLIT      0x8u           // colour * texture only (sprites, particles, sky)
#define I_ALPHATEST  0x10u          // discard below 0.5 alpha (colour key / masks)
#define I_NOTEXTURE  0x20u
#define I_CHROME     0x40u          // environment mapped / reflective
#define I_WATER      0x80u          // polygrid surface
#define I_SKY        0x100u
#define I_CLOSE      0x200u         // view model: no ray-traced self effects from the world

// texInfo flags
#define T_FULLBRIGHT 0x1u           // alpha marks fullbright texels
#define T_ALPHA      0x2u           // alpha is real alpha (4-bit masks)

struct Vertex { vec3 pos; uint color; vec2 uv; vec2 lmuv; vec3 normal; uint pad; };      // 48 bytes, scalar
struct Instance {
    vec4 r0, r1, r2;                // local -> world rows
    vec4 p0, p1, p2;                // previous frame's local -> world rows
    uint firstIndex; uint triBase; uint tex; uint flags;
    vec4 color;                     // model: ambient rgb + alpha; others: tint
    vec4 light;                     // model: directional rgb; w = reflectivity
    int  prevVertexDelta;           // dynamic geometry: prev-frame vertex = index + delta (0 = static)
    uint pad0, pad1, pad2;
};
struct Surface { uint tex; uint flags; uint pad0, pad1; };
struct Light { vec4 posRadius; vec4 color; };     // colour: signed, classic 2c-1 per channel
struct TexInfo { uint flags; float gloss; };

layout(set = 0, binding = 0) uniform sampler2D textures[MAX_TEXTURES];
layout(set = 0, binding = 1, scalar, row_major) uniform Frame {
    mat4 viewProj[3];               // 0 world, 1 sky, 2 view model
    mat4 prevViewProj[3];
    vec4 camPos;                    // xyz
    vec4 fogColor;                  // rgb, a = enabled
    vec4 fogParams;                 // near, far, skyNear, skyFar
    vec4 lightScale;                // rgb global light scale, a = saturate multiplier (1 or 2)
    vec4 modelLightDir;             // world space, towards the light
    vec4 rt;                        // enabled, shadows, reflections, ao
    vec4 misc;                      // frame index, debug view, time, exposure
    vec4 skyColor;                  // what rays see when they leave the world
    vec4 viewport;                  // scene viewport in pixels: x, y, w, h
    uint nLights; uint pad0, pad1, pad2;
} frame;
layout(set = 0, binding = 2, scalar) readonly buffer Vertices { Vertex vertices[]; };
layout(set = 0, binding = 3, scalar) readonly buffer Indices { uint indices[]; };
layout(set = 0, binding = 4, scalar) readonly buffer Instances { Instance instances[]; };
layout(set = 0, binding = 5, scalar) readonly buffer TriTable { uint triTable[]; };
layout(set = 0, binding = 6, scalar) readonly buffer Surfaces { Surface surfaces[]; };
layout(set = 0, binding = 7, scalar) readonly buffer Lights { Light lights[]; };
layout(set = 0, binding = 8, scalar) readonly buffer TexInfos { TexInfo texInfo[]; };

#define SURF_SKY 0x10u
#define SURF_BRIGHT 0x20u
#define SURF_LIGHTMAP 0x80u

vec3 xformPoint(vec4 r0, vec4 r1, vec4 r2, vec3 p) { vec4 v = vec4(p, 1.0); return vec3(dot(r0, v), dot(r1, v), dot(r2, v)); }
vec3 xformDir(vec4 r0, vec4 r1, vec4 r2, vec3 d) { return vec3(dot(r0.xyz, d), dot(r1.xyz, d), dot(r2.xyz, d)); }
