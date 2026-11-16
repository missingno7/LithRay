// Sprites, particle systems, polygrids and line systems: CPU-built dynamic geometry, following d3d.ren's rules as
// documented in blood2_recon (port_notes sprite/particles/polygrid/linesystem) and d_ren's effects_draw.d.
#include "renderer.h"
#include <algorithm>

static M4 setupTransform(const lr::ObjectRecord &o, V3 scale) {
    M4 m = quatToMatrix(o.rot.x, o.rot.y, o.rot.z, o.rot.w);
    const float s[3] = {scale.x, scale.y, scale.z};
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m.m[r][c] *= s[c];
    m.m[0][3] = o.pos.x; m.m[1][3] = o.pos.y; m.m[2][3] = o.pos.z;
    return m;
}
static uint32_t packColor(float r, float g, float b, float a) {
    auto c = [](float v) { return (uint32_t)(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}
static void setVertex(GVertex &v, V3 p, uint32_t color, float u, float t, V3 n) {
    v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z; v.color = color; v.uv[0] = u; v.uv[1] = t; v.lm[0] = v.lm[1] = 0;
    v.n[0] = n.x; v.n[1] = n.y; v.n[2] = n.z; v.pad = 0;
}
static GInstance unlitInstance(uint32_t tex, uint32_t firstIndex, uint32_t flags) {
    GInstance gi = {};
    gi.r[0][0] = gi.r[1][1] = gi.r[2][2] = 1; gi.p[0][0] = gi.p[1][1] = gi.p[2][2] = 1;
    gi.firstIndex = firstIndex; gi.tex = tex; gi.flags = I_UNLIT | flags | (tex ? 0 : I_NOTEXTURE);
    gi.color[0] = gi.color[1] = gi.color[2] = gi.color[3] = 1;
    return gi;
}

void Renderer::addSprite(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const lr::SpritePayload &sp) {
    if (settings.drawSprites == 0 || sp.tex == 0 || sp.tex >= kTexLightmap || !textures[sp.tex].img) return;
    float w = (float)textures[sp.tex].width, h = (float)textures[sp.tex].height;
    bool sky = (o.role & lr::OBJ_SKY) != 0, noZ = (o.flags & lr::FLAG_SPRITE_NOZ) != 0;
    V3 eye = sky ? skyEye : camPos;

    // d3d.ren 0x2e4c0: lit sprites ADD the light grid to the scaled object colour
    float col[3] = {o.rgba[0] * sc.lightScale.x, o.rgba[1] * sc.lightScale.y, o.rgba[2] * sc.lightScale.z};
    if (!(o.flags & lr::FLAG_NOLIGHT) && !sky) {
        V3 g = sampleGrid(V3{o.pos.x, o.pos.y, o.pos.z});
        col[0] += g.x; col[1] += g.y; col[2] += g.z;
    }
    uint32_t color = packColor(col[0] / 255, col[1] / 255, col[2] / 255, o.rgba[3] / 255.0f);

    uint32_t fv, fi;
    GVertex *v = allocDyn(4, fv);
    uint32_t *idx = v ? allocDynIndices(6, fi) : nullptr;
    if (!idx) return;
    V3 pos{o.pos.x, o.pos.y, o.pos.z}, c[4];
    V3 n = camFwd * -1.0f;
    if (o.flags & lr::FLAG_ROTATEABLESPRITE) {
        // a world-space quad of texture size through the object's matrix (decals, flat effects)
        M4 m = setupTransform(o, V3{o.scale.x, o.scale.y, o.scale.z});
        c[0] = m.point(V3{w, h, 0}); c[1] = m.point(V3{-w, h, 0}); c[2] = m.point(V3{-w, -h, 0}); c[3] = m.point(V3{w, -h, 0});
        n = normalize(m.dir(V3{0, 0, 1}));
    } else {
        float depth = dot(pos - eye, camFwd);
        if (depth <= 7.0f) { dynVerts -= 4; dynIndices -= 6; return; }
        float hx = w * o.scale.x, hy = h * o.scale.y;
        if (o.flags & lr::FLAG_GLOWSPRITE) {
            float f = std::clamp((depth - 10.0f) / 490.0f, 0.0f, 1.0f) * 1.9f + 0.1f;
            hx *= f; hy *= f;
        }
        if (o.flags & lr::FLAG_SPRITEBIAS) {
            // biased 20 units towards the viewer along the view ray, keeping its screen size
            float k = std::max(depth - 20.0f, nearZ) / depth;
            pos = eye + (pos - eye) * k; hx *= k; hy *= k;
        }
        c[0] = pos - camRight * hx + camUp * hy; c[1] = pos + camRight * hx + camUp * hy;
        c[2] = pos + camRight * hx - camUp * hy; c[3] = pos - camRight * hx - camUp * hy;
    }
    const float uv[4][2] = {{0.5f, 0.5f}, {w - 0.5f, 0.5f}, {w - 0.5f, h - 0.5f}, {0.5f, h - 0.5f}};   // half-texel inset
    for (int i = 0; i < 4; i++) setVertex(v[i], c[i], color, uv[i][0], uv[i][1], n);
    const uint32_t q[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) idx[i] = fv + q[i];
    uint32_t inst = pushInstance(unlitInstance(sp.tex, fi, sky ? I_SKY : 0));
    pushDraw(sky ? G_SKY : noZ ? G_SPRITES_NOZ : G_SPRITES, (sky || noZ) ? P_NOZ : P_BLEND, inst, fi, 6, sky ? 1 : 0);
}

void Renderer::addParticles(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload) {
    if (settings.drawParticles == 0) return;
    const lr::ParticlePayload &pp = *(const lr::ParticlePayload *)payload;
    const lr::Particle *parts = (const lr::Particle *)(payload + sizeof(lr::ParticlePayload));
    if (!pp.count) return;
    uint32_t tex = pp.tex < kTexLightmap && textures[pp.tex].img ? pp.tex : 0;
    float w = tex ? (float)textures[tex].width : 1, h = tex ? (float)textures[tex].height : 1;
    float u0 = tex ? 2.0f : 0, v0 = tex ? 2.0f : 0, u1 = tex ? w - 2.0f : 0, v1 = tex ? h - 2.0f : 0;     // two-texel inset
    M4 m = setupTransform(o, V3{o.scale.x, o.scale.y, o.scale.z});
    float half = 2.0f * pp.particleRadius;
    const float tint[3] = {o.rgba[0] * sc.lightScale.x / 255, o.rgba[1] * sc.lightScale.y / 255, o.rgba[2] * sc.lightScale.z / 255};
    bool sky = (o.role & lr::OBJ_SKY) != 0;
    V3 eye = sky ? skyEye : camPos;

    uint32_t fv, fi;
    GVertex *v = allocDyn(pp.count * 4, fv);
    uint32_t *idx = v ? allocDynIndices(pp.count * 6, fi) : nullptr;
    if (!idx) return;
    uint32_t n = 0;
    V3 normal = camFwd * -1.0f;
    for (uint32_t i = 0; i < pp.count; i++) {
        V3 p = m.point(V3{parts[i].pos.x, parts[i].pos.y, parts[i].pos.z});
        float z = dot(p - eye, camFwd);
        if (!(z > nearZ && z < farZ)) continue;         // whole particles only, like the original
        uint32_t color = packColor(parts[i].rgba[0] / 255.0f * tint[0], parts[i].rgba[1] / 255.0f * tint[1], parts[i].rgba[2] / 255.0f * tint[2],
                                   o.rgba[3] / 255.0f);
        GVertex *q = v + n * 4;
        setVertex(q[0], p - camRight * half + camUp * half, color, u0, v0, normal);
        setVertex(q[1], p + camRight * half + camUp * half, color, u1, v0, normal);
        setVertex(q[2], p + camRight * half - camUp * half, color, u1, v1, normal);
        setVertex(q[3], p - camRight * half - camUp * half, color, u0, v1, normal);
        const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) idx[n * 6 + k] = fv + n * 4 + qi[k];
        n++;
    }
    if (!n) return;
    uint32_t inst = pushInstance(unlitInstance(tex, fi, sky ? I_SKY : 0));
    pushDraw(sky ? G_SKY : G_PARTICLES, sky ? P_NOZ : P_BLEND, inst, fi, n * 6, sky ? 1 : 0);
}

void Renderer::addPolyGrid(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload) {
    if (settings.drawPolyGrids == 0) return;
    const lr::PolyGridPayload &pg = *(const lr::PolyGridPayload *)payload;
    uint32_t W = pg.width, H = pg.height;
    if (W < 2 || H < 2 || pg.nIndices < 3) return;
    const uint8_t *heights = payload + sizeof(lr::PolyGridPayload);
    const float *colors = (const float *)(heights + ((W * H + 3) & ~3u));
    const uint16_t *indices = (const uint16_t *)(colors + 256 * 4);
    bool uns = (o.flags & lr::FLAG_UNSIGNED) != 0;
    uint32_t tex = (pg.tex < kTexLightmap && textures[pg.tex].img && !(o.flags & lr::FLAG_ENVIRONMENTMAPONLY)) ? pg.tex : 0;
    float tw = tex ? (float)textures[tex].width : 64.0f, th = tex ? (float)textures[tex].height : 64.0f;

    // object matrix with the grid stretched by (n + 1) / n, then centred (d3d.ren r_DrawPolyGrid 0x2a360)
    M4 centre = M4::identity();
    centre.m[0][3] = -((float)W - 1) * 0.5f; centre.m[1][3] = uns ? -128.0f : 0.0f; centre.m[2][3] = -((float)H - 1) * 0.5f;
    M4 grid = setupTransform(o, V3{(W + 1.0f) / W * o.scale.x, o.scale.y, (H + 1.0f) / H * o.scale.z}) * centre;
    // texel-space UVs (the shader divides by the texture size)
    float uOff = pg.xPan, vOff = pg.yPan;
    float uScale = (pg.xScale != 0 ? 1.0f / pg.xScale : 0) * o.scale.x * (W / (W - 1.0f));
    float vScale = (pg.yScale != 0 ? 1.0f / pg.yScale : 0) * o.scale.z * (H / (H - 1.0f));
    (void)tw; (void)th;

    uint32_t nIdx = pg.nIndices - pg.nIndices % 3;
    uint32_t fv, fi;
    GVertex *v = allocDyn(W * H, fv);
    uint32_t *idx = v ? allocDynIndices(nIdx, fi) : nullptr;
    if (!idx) return;
    auto sample = [&](uint32_t x, uint32_t z) { uint8_t b = heights[z * W + x]; return uns ? (int)b : (int)(int8_t)b; };
    float alphaScale = o.rgba[3] / 255.0f;
    for (uint32_t z = 0; z < H; z++)
        for (uint32_t x = 0; x < W; x++) {
            int s = sample(x, z);
            const float *c = colors + (uns ? (s & 255) : s + 128) * 4;
            // surface normal from the height field (for reflections)
            float dx = (float)(sample(std::min(x + 1, W - 1), z) - sample(x > 0 ? x - 1 : 0, z));
            float dz = (float)(sample(x, std::min(z + 1, H - 1)) - sample(x, z > 0 ? z - 1 : 0));
            V3 n = normalize(grid.dir(V3{-dx * 0.5f, 1.0f, -dz * 0.5f}));
            uint32_t color = packColor(c[0] * sc.lightScale.x / 255, c[1] * sc.lightScale.y / 255, c[2] * sc.lightScale.z / 255, c[3] * alphaScale / 255);
            setVertex(v[z * W + x], grid.point(V3{(float)x, (float)s, (float)z}), color, (x + uOff / tw) * uScale, (z + vOff / th) * vScale, n);
        }
    for (uint32_t i = 0; i < nIdx; i++) idx[i] = fv + (indices[i] < W * H ? indices[i] : 0);

    bool sky = (o.role & lr::OBJ_SKY) != 0, translucent = o.rgba[3] != 255;
    GInstance gi = unlitInstance(tex, fi, I_WATER | (sky ? I_SKY : 0));
    gi.light[3] = 0.55f;        // reflectivity when ray tracing is on (replaces d3d.ren's environment-map pass)
    History &hist = historyMap[o.key];
    if (hist.frame + 1 == frameIndex && hist.nVertices == W * H) gi.prevVertexDelta = (int32_t)hist.firstVertex - (int32_t)fv;
    hist.firstVertex = fv; hist.nVertices = W * H; hist.frame = frameIndex;
    uint32_t inst = pushInstance(gi);
    pushDraw(sky ? G_SKY : translucent ? G_TRANS_POLYGRIDS : G_SOLID_POLYGRIDS, sky ? P_NOZ : translucent ? P_BLEND : P_OPAQUE, inst, fi, nIdx, sky ? 1 : 0);
}

void Renderer::addLines(const lr::SceneCmd &, const lr::ObjectRecord &o, const uint8_t *payload) {
    if (settings.drawLineSystems == 0) return;
    const lr::LinePayload &lp = *(const lr::LinePayload *)payload;
    const lr::LineSeg *segs = (const lr::LineSeg *)(payload + sizeof(lr::LinePayload));
    if (!lp.count) return;
    M4 m = setupTransform(o, V3{o.scale.x, o.scale.y, o.scale.z});
    uint32_t fv, fi;
    GVertex *v = allocDyn(lp.count * 4, fv);
    uint32_t *idx = v ? allocDynIndices(lp.count * 6, fi) : nullptr;
    if (!idx) return;
    float a = o.rgba[3] / 255.0f;
    for (uint32_t i = 0; i < lp.count; i++) {
        // one-pixel lines become thin camera-facing quads (about 1.5 px at any distance)
        V3 p0 = m.point(V3{segs[i].a.x, segs[i].a.y, segs[i].a.z}), p1 = m.point(V3{segs[i].b.x, segs[i].b.y, segs[i].b.z});
        V3 side0 = normalize(cross(p1 - p0, p0 - camPos)), side1 = side0;
        float w0 = std::max(dot(p0 - camPos, camFwd), nearZ) * tanY * 1.5f / std::max(sceneViewport.height, 1.0f);
        float w1 = std::max(dot(p1 - camPos, camFwd), nearZ) * tanY * 1.5f / std::max(sceneViewport.height, 1.0f);
        uint32_t c0 = packColor(segs[i].ca[0], segs[i].ca[1], segs[i].ca[2], segs[i].ca[3] * a);
        uint32_t c1 = packColor(segs[i].cb[0], segs[i].cb[1], segs[i].cb[2], segs[i].cb[3] * a);
        V3 n = camFwd * -1.0f;
        GVertex *q = v + i * 4;
        setVertex(q[0], p0 - side0 * w0, c0, 0, 0, n); setVertex(q[1], p0 + side0 * w0, c0, 0, 0, n);
        setVertex(q[2], p1 + side1 * w1, c1, 0, 0, n); setVertex(q[3], p1 - side1 * w1, c1, 0, 0, n);
        const uint32_t qi[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) idx[i * 6 + k] = fv + i * 4 + qi[k];
    }
    uint32_t inst = pushInstance(unlitInstance(0, fi, 0));
    pushDraw(G_LINES, P_BLEND, inst, fi, lp.count * 6);
}
