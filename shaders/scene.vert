#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec2 inLm;
layout(location = 4) in vec3 inNormal;

layout(push_constant) uniform PC { uint instance; uint view; } pc;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec2 vLm;
layout(location = 4) out vec4 vColor;
layout(location = 5) out vec4 vClip;
layout(location = 6) out vec4 vPrevClip;
layout(location = 7) flat out uint vInstance;

void main() {
    Instance I = instances[pc.instance];
    vec3 world = xformPoint(I.r0, I.r1, I.r2, inPos);
    vec3 prevLocal = inPos;
    if (I.prevVertexDelta != 0) prevLocal = vertices[gl_VertexIndex + I.prevVertexDelta].pos;
    vec3 prevWorld = xformPoint(I.p0, I.p1, I.p2, prevLocal);
    vWorld = world;
    vNormal = xformDir(I.r0, I.r1, I.r2, inNormal);
    vUv = inUv; vLm = inLm; vColor = inColor; vInstance = pc.instance;
    gl_Position = frame.viewProj[pc.view] * vec4(world, 1.0);
    vClip = gl_Position;
    vPrevClip = frame.prevViewProj[pc.view] * vec4(prevWorld, 1.0);
}
