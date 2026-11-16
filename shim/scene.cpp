// Snapshots of engine memory, taken inside the slot call (nothing here may be read after the slot returns):
// the world at CreateContext, and per RenderScene the camera, polled surface state and EVERY client object of
// the world tree -- not only the visible ones, because ray tracing needs geometry outside the view.
// Layouts and semantics: shim/lt1_abi.h and blood2_recon docs/renderer_closure (A-E).
#include "shim.h"
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

using lr::Vec3;

// ------------------------------------------------------------------ growable output buffer
struct Out {
    std::vector<uint8_t> d;
    uint32_t size() const { return (uint32_t)d.size(); }
    void *grow(uint32_t n) { size_t at = d.size(); d.resize(at + n); return d.data() + at; }
    template <class T> uint32_t put(const T &v) { uint32_t at = size(); memcpy(grow(sizeof(T)), &v, sizeof(T)); return at; }
    void bytes(const void *p, uint32_t n) { if (n) memcpy(grow(n), p, n); }
    void align(uint32_t a) { while (d.size() % a) d.push_back(0); }
    template <class T> T *at(uint32_t off) { return (T *)(d.data() + off); }
};

// ------------------------------------------------------------------ context / world
struct RenderContext { void *unknown0; int unknown4; LT1MainWorld *mainWorld; uint16_t frameCode; };  // d3d.ren's layout head

struct BspRef { LT1WorldBsp *bsp; uint32_t key; uint32_t firstSurface, firstPoly, firstVertex; };
static std::vector<BspRef> g_bsps;
static std::vector<float> g_uvCache;           // u, v per world vertex, as last sent
static LT1MainWorld *g_world;
static uint32_t g_contextId, g_nSurfaces, g_nPolys, g_nVertices;
static std::unordered_map<uint32_t, uint32_t> g_bspByWorldData;   // WorldData* -> bsp index

static Vec3 V(const LT1Vector &v) { return Vec3{v.x, v.y, v.z}; }

// The world file the single-player server loaded: g_pServerMgr (CLIENT.EXE 2.1 RVA 0x91728) +0x1a4 m_pWorldFile, a
// UsedFile whose first field is a hash element keyed by the file name (length at +0x14, characters at +0x16).
// Only the world FILE still has the level's static lights; the engine never creates them (CF_NORUNTIME).
static void LoadedWorldFile(char *out, size_t size) {
    out[0] = 0;
    __try {
        uint8_t *exe = (uint8_t *)GetModuleHandleA(nullptr);
        uint8_t *server = *(uint8_t **)(exe + 0x91728);
        if (!server || IsBadReadPtr(server + 0x1a4, 4)) return;
        uint8_t *used = *(uint8_t **)(server + 0x1a4);
        if (!used || IsBadReadPtr(used, 4)) return;
        uint8_t *element = *(uint8_t **)used;
        if (!element || IsBadReadPtr(element + 0x14, 2)) return;
        uint16_t len = *(uint16_t *)(element + 0x14);
        if (!len || len > 260 || IsBadReadPtr(element + 0x16, len)) return;
        size_t n = 0;
        for (; n < len && n + 1 < size && element[0x16 + n]; n++) out[n] = (char)element[0x16 + n];
        out[n] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}

static void RezSources(char *out, size_t size) {
    out[0] = 0;
    const char *cl = GetCommandLineA();
    std::vector<std::string> tokens; std::string cur; bool quoted = false, any = false;
    for (const char *c = cl; *c; c++) {
        if (*c == '"') { quoted = !quoted; any = true; }
        else if ((*c == ' ' || *c == '	') && !quoted) { if (any) tokens.push_back(cur); cur.clear(); any = false; }
        else { cur += *c; any = true; }
    }
    if (any) tokens.push_back(cur);
    std::string joined;
    for (size_t i = 0; i + 1 < tokens.size(); i++)
        if (!_stricmp(tokens[i].c_str(), "-rez")) { if (!joined.empty()) joined += ';'; joined += tokens[i + 1]; }
    strncpy(out, joined.c_str(), size - 1); out[size - 1] = 0;
}

static void SendWorld(LT1MainWorld *world) {
    g_bsps.clear(); g_bspByWorldData.clear(); g_uvCache.clear();
    g_world = world; g_nSurfaces = g_nPolys = g_nVertices = 0;
    if (!world || !world->m_pWorldBsp) return;

    g_bsps.push_back({world->m_pWorldBsp, 0});
    for (uint32_t i = 0; i < world->m_nWorldModels; i++) {
        LT1WorldData *wd = world->m_WorldModels[i];
        if (!wd || !wd->m_pOriginalBsp) continue;
        g_bspByWorldData[(uint32_t)(uintptr_t)wd] = (uint32_t)g_bsps.size();
        g_bsps.push_back({wd->m_pOriginalBsp, (uint32_t)(uintptr_t)wd});   // drawn from the ORIGINAL bsp through m_Transform
    }

    Out bspsOut, surfOut, polyOut, vertOut, lmOut;
    for (BspRef &b : g_bsps) {
        LT1WorldBsp *bsp = b.bsp;
        b.firstSurface = g_nSurfaces; b.firstPoly = g_nPolys; b.firstVertex = g_nVertices;
        for (uint32_t s = 0; s < bsp->m_nSurfaces; s++) {
            const LT1Surface &sf = bsp->m_Surfaces[s];
            lr::WorldSurface ws{sf.m_Flags, sf.m_pPlane ? V(sf.m_pPlane->m_Normal) : Vec3{0, 1, 0}};
            surfOut.put(ws);
        }
        uint32_t nPolys = 0;
        for (uint32_t p = 0; p < bsp->m_nPolies; p++) {
            LT1WorldPoly *poly = bsp->m_Polies[p];
            if (!poly || !poly->m_pSurface) continue;
            LT1Surface *sf = poly->m_pSurface;
            lr::WorldPoly wp{};
            wp.surface = b.firstSurface + (uint32_t)(sf - bsp->m_Surfaces);
            wp.firstVertex = g_nVertices; wp.nVertices = poly->m_nVertices;
            wp.lmOffset = 0xffffffffu;
            if (poly->m_pLightmap && (sf->m_Flags & lr::SURF_LIGHTMAP)) {
                // The engine frees the main world's lightmap source before DeleteContext: copy it now.
                uint8_t w = poly->m_pLightmap[0], h = poly->m_pLightmap[1];
                if (w && h) {
                    wp.lmOffset = lmOut.size(); wp.lmWidth = w; wp.lmHeight = h;
                    lmOut.bytes(poly->m_pLightmap + 2, (uint32_t)w * h * 2);
                }
            }
            for (uint32_t v = 0; v < poly->m_nVertices; v++) {
                const LT1SPolyVertex &sv = poly->m_Vertices[v];
                lr::WorldVertex wv{};
                wv.pos = V(*sv.m_pPoint);
                wv.u = sv.m_U; wv.v = sv.m_V;
                // d3d.ren 0x374c2: 20 world units per lightmap texel, sampled at texel centres
                LT1Vector d{sv.m_pPoint->x - poly->m_LMOrigin.x, sv.m_pPoint->y - poly->m_LMOrigin.y, sv.m_pPoint->z - poly->m_LMOrigin.z};
                const LT1Vector &a = sf->m_LMVecs[0], &bb = sf->m_LMVecs[1];
                wv.lmu = (d.x * a.x + d.y * a.y + d.z * a.z) * 0.05f + 0.5f;
                wv.lmv = (d.x * bb.x + d.y * bb.y + d.z * bb.z) * 0.05f + 0.5f;
                memcpy(wv.color, sv.m_Color, 4);
                vertOut.put(wv);
                g_uvCache.push_back(sv.m_U); g_uvCache.push_back(sv.m_V);
            }
            polyOut.put(wp);
            g_nVertices += poly->m_nVertices;
            nPolys++;
        }
        g_nSurfaces += bsp->m_nSurfaces;
        lr::WorldBsp wb{b.firstSurface, bsp->m_nSurfaces, b.firstPoly, nPolys, b.key, 0};
        bspsOut.put(wb);
        g_nPolys += nPolys;
    }
    lmOut.align(8);

    const LT1LightTable &lt = world->m_LightTable;
    uint32_t nSamples = lt.m_pData ? (uint32_t)(lt.m_Dims[0] * lt.m_Dims[1] * lt.m_Dims[2]) : 0;
    uint32_t extra = bspsOut.size() + surfOut.size() + polyOut.size() + vertOut.size() + lmOut.size() + nSamples * 4;
    auto *c = bridge::Emit<lr::WorldBeginCmd>(lr::CmdWorldBegin, extra);
    c->contextId = g_contextId;
    c->nBsps = (uint32_t)g_bsps.size(); c->nSurfaces = g_nSurfaces; c->nPolys = g_nPolys; c->nVertices = g_nVertices;
    c->lightmapBytes = lmOut.size();
    memcpy(c->grid.dims, lt.m_Dims, sizeof(c->grid.dims));
    c->grid.blockSize = V(lt.m_BlockSize); c->grid.lookupStart = V(lt.m_LookupStart); c->grid.nSamples = nSamples;
    strncpy(c->worldName, world->m_pWorldBsp->m_WorldName, sizeof(c->worldName) - 1);
    LoadedWorldFile(c->worldFile, sizeof(c->worldFile));
    RezSources(c->rezSources, sizeof(c->rezSources));
    uint8_t *p = (uint8_t *)(c + 1);
    auto copy = [&](Out &o) { memcpy(p, o.d.data(), o.size()); p += o.size(); };
    copy(bspsOut); copy(surfOut); copy(polyOut); copy(vertOut); copy(lmOut);
    if (nSamples) memcpy(p, lt.m_pData, nSamples * 4);
    bridge::Log("world '%s' (%s): %u bsps, %u surfaces, %u polys, %u vertices, %u lightmap bytes, grid %dx%dx%d",
                c->worldName, c->worldFile, c->nBsps, g_nSurfaces, g_nPolys, g_nVertices, c->lightmapBytes, lt.m_Dims[0], lt.m_Dims[1], lt.m_Dims[2]);
}

// ------------------------------------------------------------------ models
struct ModelEntry { uint32_t id, nVerts, nFaces, nNodes; char *filename; };
static std::unordered_map<LT1Model *, ModelEntry> g_models;
static uint32_t g_nextModelId = 1;

static void FreeModels() {
    for (auto &m : g_models) { auto *c = bridge::Emit<lr::ModelFreeCmd>(lr::CmdModelFree); c->id = m.second.id; }
    g_models.clear();
}

static uint32_t ModelId(LT1Model *model) {
    uint32_t nVerts = model->m_nBaseVertices, nFaces = model->m_nFaces, nNodes = model->m_nNodeMatrices;
    auto it = g_models.find(model);
    if (it != g_models.end()) {
        ModelEntry &e = it->second;     // no unload notification exists: validate what can be validated
        if (e.nVerts == nVerts && e.nFaces == nFaces && e.nNodes == nNodes && e.filename == model->m_pFilename) return e.id;
        auto *f = bridge::Emit<lr::ModelFreeCmd>(lr::CmdModelFree); f->id = e.id;
        g_models.erase(it);
    }
    if (!model->m_Vertices || !model->m_Faces || !model->m_FaceUVs || !nVerts || !nFaces || nNodes == 0 || nNodes > 255) {
        static std::unordered_set<LT1Model *> reported;
        if (reported.insert(model).second)
            bridge::Log("model %s rejected: verts %p/%u faces %p/%u uvs %p nodes %u", model->m_pFilename ? model->m_pFilename : "?",
                        model->m_Vertices, nVerts, model->m_Faces, nFaces, model->m_FaceUVs, nNodes);
        return 0;
    }
    uint32_t extra = nVerts * sizeof(lr::ModelVertex) + nFaces * sizeof(lr::ModelFace);
    auto *c = bridge::Emit<lr::ModelDefCmd>(lr::CmdModelDef, extra);
    c->id = g_nextModelId++; c->nVertices = nVerts; c->nFaces = nFaces; c->nNodes = nNodes;
    if (model->m_pFilename) strncpy(c->name, model->m_pFilename, sizeof(c->name) - 1);
    lr::ModelVertex *v = (lr::ModelVertex *)(c + 1);
    for (uint32_t i = 0; i < nVerts; i++) {
        const LT1ModelVertex &s = model->m_Vertices[i];
        v[i].pos = Vec3{s.m_Pos[0], s.m_Pos[1], s.m_Pos[2]};
        memcpy(v[i].normal, s.m_Normal, 3);
        v[i].node = s.m_NodeIndex < nNodes ? s.m_NodeIndex : 0;
    }
    lr::ModelFace *f = (lr::ModelFace *)(v + nVerts);
    for (uint32_t i = 0; i < nFaces; i++) {
        const LT1ModelFace &s = model->m_Faces[i];
        for (int k = 0; k < 3; k++) f[i].v[k] = s.m_Vertices[k] < nVerts ? s.m_Vertices[k] : 0;
        f[i].nodeSelector = s.m_NodeSelector;
        memcpy(f[i].uv, model->m_FaceUVs + i * 6, sizeof(f[i].uv));
    }
    g_models[model] = ModelEntry{c->id, nVerts, nFaces, nNodes, model->m_pFilename};
    bridge::Log("model %u: %s (%u verts, %u/%u total, %u faces, %u nodes, %u lods)", c->id, c->name, nVerts, model->m_nVertices, nVerts, nFaces, nNodes, model->m_nLODs);
    return c->id;
}

// Row-major 3x4 (rotation | translation), column vectors.
struct M34 { float m[3][4]; };
static M34 Mul(const M34 &a, const M34 &b) {
    M34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        r.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
    }
    return r;
}
static M34 QuatToM(const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M34 r = {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0},
              {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0},
              {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0}}};
    return r;
}
static void Slerp(const float *a, const float *bIn, float t, float *out) {
    float dot = a[0] * bIn[0] + a[1] * bIn[1] + a[2] * bIn[2] + a[3] * bIn[3];
    float b[4] = {bIn[0], bIn[1], bIn[2], bIn[3]};
    if (dot < 0) { dot = -dot; for (float &c : b) c = -c; }
    float sa = 1 - t, sb = t;
    if (1.0 - dot > 0.00001f) {     // not renormalised, like d3d.ren
        float omega = atan2f(sqrtf((1 + dot) * (1 - dot)), dot), so = sinf(omega);
        sa = sinf((1 - t) * omega) / so; sb = sinf(t * omega) / so;
    }
    for (int i = 0; i < 4; i++) out[i] = sa * a[i] + sb * b[i];
}

// d3d.ren pose builder (0x3e710 / 0x3e2d0): pre-order parent * local, key quaternions through the TRANSPOSED
// quat-to-matrix (= the conjugate), deformation nodes get their dequantisation affine after the children.
struct Pose {
    uint32_t keyA, keyB; float t; M34 *out; uint32_t count;
    uint32_t Node(LT1AnimNode *a, LT1AnimNode *b, const M34 &parent, uint32_t index) {
        if (index >= count) return index;
        if (!a->m_KeyFrames || !b->m_KeyFrames || !a->m_pNode) { out[index] = parent; return index + 1; }
        const LT1NodeKeyFrame &ka = a->m_KeyFrames[keyA], &kb = b->m_KeyFrames[keyB];
        float q[4];
        Slerp(&ka.m_Quat.x, &kb.m_Quat.x, t, q);
        float conj[4] = {-q[0], -q[1], -q[2], q[3]};
        M34 local = QuatToM(conj);
        local.m[0][3] = ka.m_Translation.x + (kb.m_Translation.x - ka.m_Translation.x) * t;
        local.m[1][3] = ka.m_Translation.y + (kb.m_Translation.y - ka.m_Translation.y) * t;
        local.m[2][3] = ka.m_Translation.z + (kb.m_Translation.z - ka.m_Translation.z) * t;
        out[index] = Mul(parent, local);
        uint32_t next = index + 1;
        for (uint32_t i = 0; i < a->m_pNode->m_nChildren; i++) next = Node(a->m_Children + i, b->m_Children + i, out[index], next);
        if (a->m_pNode->m_Flags & 4) {
            const float *fa = &a->m_Scale.x, *fb = &b->m_Scale.x;   // scale diagonal, then translation
            float v[6];
            for (int i = 0; i < 6; i++) v[i] = fa[i] + (fb[i] - fa[i]) * t;
            M34 aff = {{{v[0], 0, 0, v[3]}, {0, v[1], 0, v[4]}, {0, 0, v[2], v[5]}}};
            out[index] = Mul(out[index], aff);
        }
        return next;
    }
};

// d3d.ren 0x3f200: per-vertex animation rewrites the model's shared vertex positions every frame.
static bool BlendVertexAnimation(LT1Model *model, LT1ModelAnim *animA, LT1ModelAnim *animB, uint32_t keyA, uint32_t keyB, float t) {
    uint32_t nNodes = model->m_nFlaggedNodes, nVerts = model->m_nBaseVertices;
    void **infoA = (void **)animA->m_pFlaggedNodeData, **infoB = (void **)animB->m_pFlaggedNodeData;
    if (!nNodes || !model->m_Vertices || !infoA || !infoB) return false;
    int q14 = (int)(t * 16384.0f);
    bool any = false;
    for (uint32_t n = 0; n < nNodes; n++) {
        uint8_t *ia = (uint8_t *)infoA[n], *ib = (uint8_t *)infoB[n];
        if (!ia || !ib) continue;
        uint8_t *da = *(uint8_t **)ia, *db = *(uint8_t **)ib;       // the ModelNode
        if (!da || !db) continue;
        uint32_t countA = *(uint32_t *)(da + 0x24), countB = *(uint32_t *)(db + 0x24);
        const uint8_t *pa = *(uint8_t **)(ia + 8), *pb = *(uint8_t **)(ib + 8);
        const uint16_t *map = *(uint16_t **)(da + 0x20);
        if (!pa || !pb || !map) continue;
        pa += keyA * countA * 3; pb += keyB * countB * 3;
        for (uint32_t i = 0; i < countA; i++, pa += 3, pb += 3) {
            if (map[i] >= nVerts) continue;
            float *dst = model->m_Vertices[map[i]].m_Pos;
            for (int k = 0; k < 3; k++) dst[k] = (float)(((int)pa[k] << 14) + ((int)pb[k] - (int)pa[k]) * q14) * (1.0f / 16384.0f);
        }
        any = true;
    }
    return any;
}

// ------------------------------------------------------------------ objects
static Out g_obj;
static uint32_t g_nObjects;
static std::unordered_set<LT1DObject *> g_seen;
static LT1SceneDesc *g_scene;
static float g_camFwd[3], g_camPos[3];
static std::vector<M34> g_pose;
#define FLAG_INTERNAL1 0x20000000u

static bool NearView(const LT1Vector &c, float radius) {
    // Coarse stand-in for d3d.ren's PVS + frustum test, only for the "was drawn" feedback: in front of the
    // camera plane (with slack) and inside the far distance.
    float d[3] = {c.x - g_camPos[0], c.y - g_camPos[1], c.z - g_camPos[2]};
    float z = d[0] * g_camFwd[0] + d[1] * g_camFwd[1] + d[2] * g_camFwd[2];
    return z > -radius && z < g_scene->m_FarZ + radius;
}

static uint32_t BeginRecord(LT1DObject *o, uint8_t role) {
    lr::ObjectRecord r{};
    r.type = (uint8_t)o->m_ObjectType; r.role = role;
    r.key = (uint32_t)(uintptr_t)o;
    r.flags = o->m_Flags; r.userFlags = o->m_UserFlags;
    r.rgba[0] = o->m_ColorR; r.rgba[1] = o->m_ColorG; r.rgba[2] = o->m_ColorB; r.rgba[3] = o->m_ColorA;
    r.pos = V(o->m_Pos); r.rot = lr::Quat{o->m_Rotation.x, o->m_Rotation.y, o->m_Rotation.z, o->m_Rotation.w};
    r.scale = V(o->m_Scale); r.radius = o->m_Radius;
    return g_obj.put(r);
}
static void EndRecord(uint32_t at) {
    g_obj.align(8);
    g_obj.at<lr::ObjectRecord>(at)->size = g_obj.size() - at;
    g_nObjects++;
}

static void EmitModel(LT1DObject *o, uint8_t role) {
    LT1ModelInstance *mi = (LT1ModelInstance *)o;
    LT1AnimTracker &tr = mi->m_AnimTracker;
    LT1Model *model = tr.m_pModel;
    LT1ModelAnim *animA = tr.m_KeyA.m_pAnim, *animB = tr.m_KeyB.m_pAnim;
    if (!model || !animA || !animB) {
        static int n; if (n++ < 20) bridge::Log("model object %p skipped: model %p animA %p animB %p", o, model, animA, animB);
        return;
    }
    uint32_t id = ModelId(model);
    if (!id) return;
    uint32_t nNodes = model->m_nNodeMatrices;
    float t = tr.m_Percent;
    if (g_pose.size() < nNodes) g_pose.resize(nNodes);
    M34 identity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    for (uint32_t i = 0; i < nNodes; i++) g_pose[i] = identity;
    Pose pose{tr.m_KeyA.m_iKey, tr.m_KeyB.m_iKey, t, g_pose.data(), nNodes};
    pose.Node(&animA->m_RootNode, &animB->m_RootNode, identity, 0);
    bool deform = BlendVertexAnimation(model, animA, animB, tr.m_KeyA.m_iKey, tr.m_KeyB.m_iKey, t);

    // Ambient as d3d.ren's model frame state builds it, then the game's model hook (once per model per scene).
    // Dynamic lights are added by the host per pixel instead of into this ambient term.
    lr::ModelPayload mp{};
    mp.modelId = id;
    float amb[3] = {(float)o->m_ColorR, (float)o->m_ColorG, (float)o->m_ColorB};
    const float *add = &g_scene->m_GlobalModelLightAdd.x, *scale = &g_scene->m_GlobalLightScale.x;
    for (int i = 0; i < 3; i++) {
        amb[i] = (amb[i] + add[i]) * scale[i];
        amb[i] = amb[i] < 0 ? 0 : amb[i] > 255 ? 255 : amb[i];
    }
    uint32_t drawFlags = o->m_Flags, hookFlags = lr::MHF_USETEXTURE;
    if (g_scene->m_ModelHookFn) {
        LT1Vector la{amb[0], amb[1], amb[2]};
        LT1ModelHookData hd{o, lr::MHF_USETEXTURE, o->m_Flags, &la};
        g_scene->m_ModelHookFn(&hd, g_scene->m_ModelHookUser);
        amb[0] = la.x; amb[1] = la.y; amb[2] = la.z;
        drawFlags = hd.m_ObjectFlags; hookFlags = hd.m_Flags;
    }
    mp.hookFlags = hookFlags; mp.lightAdd = Vec3{amb[0], amb[1], amb[2]};
    mp.skinTex = TexId(mi->m_pSkin);
    mp.nNodes = nNodes;
    mp.nDeform = deform ? model->m_nBaseVertices : 0;
    uint32_t nHidden = 0;
    if (mi->m_HiddenNodes) while (mi->m_HiddenNodes[nHidden] != 0xff && nHidden < 255) nHidden++;
    mp.nHidden = nHidden;

    uint32_t at = BeginRecord(o, role);
    g_obj.at<lr::ObjectRecord>(at)->flags = drawFlags;
    g_obj.put(mp);
    g_obj.bytes(g_pose.data(), nNodes * sizeof(M34));
    if (deform) for (uint32_t i = 0; i < model->m_nBaseVertices; i++) g_obj.bytes(model->m_Vertices[i].m_Pos, 12);
    g_obj.bytes(mi->m_HiddenNodes, nHidden);
    EndRecord(at);
}

static void EmitObject(LT1DObject *o, uint8_t role) {
    if (!o || !(o->m_Flags & lr::FLAG_VISIBLE)) return;
    if (!g_seen.insert(o).second) return;
    switch (o->m_ObjectType) {
    case LT1_OT_MODEL: EmitModel(o, role); break;
    case LT1_OT_WORLDMODEL: case LT1_OT_CONTAINER: {
        LT1WorldModelInstance *w = (LT1WorldModelInstance *)o;
        auto it = g_bspByWorldData.find((uint32_t)(uintptr_t)w->m_pWorldData);
        if (it == g_bspByWorldData.end()) break;
        uint32_t at = BeginRecord(o, role);
        lr::WorldModelPayload p{it->second, 0};
        memcpy(p.transform, w->m_Transform.m, sizeof(p.transform));
        g_obj.put(p);
        EndRecord(at);
        break;
    }
    case LT1_OT_SPRITE: {
        LT1SpriteInstance *s = (LT1SpriteInstance *)o;
        LT1SharedTexture *tex = s->m_Tracker.m_pCurFrame ? *s->m_Tracker.m_pCurFrame : nullptr;
        uint32_t id = TexId(tex);
        if (!id) break;
        uint32_t at = BeginRecord(o, role);
        uint32_t pAt = g_obj.put(lr::SpritePayload{id, 0});
        // Decals: d3d.ren clips rotatable sprites to the world polygon named by the handle (high word: world model
        // index, 0xffff = main world; low word: node index). Resolved here, where the BSP is readable.
        uint32_t h = s->m_ClipperPoly;
        if ((o->m_Flags & lr::FLAG_ROTATEABLESPRITE) && h != 0xffffffffu && h != 0 && g_world) {
            LT1WorldBsp *bsp = nullptr;
            if ((h >> 16) == 0xffff) bsp = g_world->m_pWorldBsp;
            else if ((h >> 16) < g_world->m_nWorldModels && g_world->m_WorldModels[h >> 16]) bsp = g_world->m_WorldModels[h >> 16]->m_pWorldBsp;
            LT1WorldPoly *poly = (bsp && bsp->m_Nodes && (h & 0xffff) < bsp->m_nNodes) ? bsp->m_Nodes[h & 0xffff].m_pPoly : nullptr;
            if (poly && poly->m_pSurface && poly->m_pSurface->m_pPlane && poly->m_nVertices >= 3 && poly->m_nVertices < 64) {
                g_obj.put(*poly->m_pSurface->m_pPlane);
                for (uint32_t v = 0; v < poly->m_nVertices; v++) g_obj.put(*poly->m_Vertices[v].m_pPoint);
                g_obj.at<lr::SpritePayload>(pAt)->nClip = poly->m_nVertices;
            } else
                g_obj.at<lr::SpritePayload>(pAt)->nClip = 0xffffffffu;      // unresolved handle: d3d.ren draws nothing
        }
        EndRecord(at);
        break;
    }
    case LT1_OT_LIGHT: {
        uint32_t at = BeginRecord(o, role);
        g_obj.put(lr::LightPayload{((LT1DynamicLight *)o)->m_LightRadius, 0});
        EndRecord(at);
        break;
    }
    case LT1_OT_PARTICLESYSTEM: {
        LT1ParticleSystem *ps = (LT1ParticleSystem *)o;
        // The engine only keeps updating systems the renderer reported as drawn.
        if (NearView(ps->m_SystemCenter, ps->m_SystemRadius)) o->m_Flags |= FLAG_INTERNAL1;
        uint32_t at = BeginRecord(o, role);
        uint32_t pAt = g_obj.put(lr::ParticlePayload{TexId(ps->m_pTexture), 0, ps->m_ParticleRadius, ps->m_PSFlags});
        uint32_t count = 0;
        for (LT1PSParticle *p = ps->m_ParticleHead.m_pNext; p && p != &ps->m_ParticleHead && count < 20000; p = p->m_pNext) {
            lr::Particle out{V(p->m_Pos), {}};
            for (int i = 0; i < 3; i++) { float c = (&p->m_Color.x)[i]; out.rgba[i] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c); }
            out.rgba[3] = o->m_ColorA;
            g_obj.put(out);
            count++;
        }
        g_obj.at<lr::ParticlePayload>(pAt)->count = count;
        EndRecord(at);
        break;
    }
    case LT1_OT_POLYGRID: {
        LT1PolyGrid *pg = (LT1PolyGrid *)o;
        if (!pg->m_Data || !pg->m_pIndices || !pg->m_Width || !pg->m_Height || pg->m_Width * pg->m_Height > 65536) break;
        if (NearView(o->m_Pos, o->m_Radius)) o->m_Flags |= FLAG_INTERNAL1;
        LT1SharedTexture **frame = *(LT1SharedTexture ***)(pg->m_SpriteTracker + 8);
        uint32_t at = BeginRecord(o, role);
        lr::PolyGridPayload p{TexId(frame ? *frame : nullptr), TexId(pg->m_pEnvMap), pg->m_Width, pg->m_Height,
                              pg->m_xPan, pg->m_yPan, pg->m_xScale, pg->m_yScale, pg->m_nIndices, 0};
        g_obj.put(p);
        g_obj.bytes(pg->m_Data, pg->m_Width * pg->m_Height); g_obj.align(4);
        g_obj.bytes(pg->m_ColorTable, sizeof(pg->m_ColorTable));
        g_obj.bytes(pg->m_pIndices, pg->m_nIndices * 2); g_obj.align(4);
        EndRecord(at);
        break;
    }
    case LT1_OT_LINESYSTEM: {
        LT1LineSystem *ls = (LT1LineSystem *)o;
        uint32_t at = BeginRecord(o, role);
        uint32_t pAt = g_obj.put(lr::LinePayload{0, 0});
        uint32_t count = 0;
        for (LT1LineSegment *s = ls->m_LineHead.m_pNext; s && s != &ls->m_LineHead && count < 20000; s = s->m_pNext) {
            g_obj.bytes(s, 0x38);       // {start, colour, end, colour} == lr::LineSeg
            count++;
        }
        g_obj.at<lr::LinePayload>(pAt)->count = count;
        EndRecord(at);
        break;
    }
    default: break;
    }
}

static void EmitWithAttachments(LT1DObject *o, uint8_t role) {
    if (!o || !(o->m_Flags & lr::FLAG_VISIBLE)) return;
    EmitObject(o, role);
    // The engine positions attachment children (weapons in hands, ...) when asked, one level deep like d3d.ren.
    for (LT1Attachment *a = o->m_Attachments; a; a = a->m_pNext)
        EmitObject((LT1DObject *)g_rs->GetAttachmentObject(o, a), role | lr::OBJ_ATTACHMENT);
}

static void CollectWorldObjects(LT1WorldBsp *bsp) {
    // Every FastNode list from the tail: client objects sit at the tail, the first server object ends the walk
    // (d3d.ren r_CollectLeafObjects 0x3f3f0; this is its DrawAll path, not the PVS-limited one).
    LT1FastNode *nodes = (LT1FastNode *)bsp->m_FastNodes;
    if (nodes)
        for (uint32_t i = 0; i < bsp->m_nFastNodes; i++) {
            LT1Link *head = &nodes[i].m_Objects;
            uint32_t guard = 0;
            for (LT1Link *l = head->m_pPrev; l && l != head && guard < 100000; l = l->m_pPrev, guard++) {
                LT1DObject *o = (LT1DObject *)l->m_pData;
                if (!o) continue;
                if (o->sd) break;
                if (o->m_ObjectType != LT1_OT_POLYGRID && !(o->m_Flags & lr::FLAG_SKYOBJECT)) EmitWithAttachments(o, 0);
            }
        }
    // PolyGrids are linked into leaves instead.
    if (bsp->m_Leafs)
        for (uint32_t i = 0; i < bsp->m_nLeafs; i++) {
            LT1Link *head = &bsp->m_Leafs[i].m_LeafLinks;
            uint32_t guard = 0;
            for (LT1Link *l = head->m_pNext; l && l != head && guard < 100000; l = l->m_pNext, guard++) {
                if (!l->m_pData) continue;
                LT1DObject *o = *(LT1DObject **)((uint8_t *)l->m_pData + 0x18);
                if (o && o->m_ObjectType == LT1_OT_POLYGRID && !(o->m_Flags & lr::FLAG_SKYOBJECT)) EmitWithAttachments(o, 0);
            }
        }
}

static void FillSettings(lr::Settings &s) {
    s.fogEnable = CVar("FogEnable", 0); s.fogNearZ = CVar("FogNearZ", 0); s.fogFarZ = CVar("FogFarZ", 5000);
    s.fogR = CVar("FogR", 0); s.fogG = CVar("FogG", 0); s.fogB = CVar("FogB", 0);
    s.skyFogNearZ = CVar("SkyFogNearZ", 100); s.skyFogFarZ = CVar("SkyFogFarZ", 1000); s.skyScale = CVar("SkyScale", 1);
    s.saturate = CVar("Saturate", 0); s.lightSaturate = CVar("LightSaturate", 1); s.lightModels = CVar("LightModels", 1);
    s.dynamicLight = CVar("DynamicLight", 1); s.envMapEnable = CVar("EnvMapEnable", 0); s.envMapAll = CVar("EnvMapAll", 0);
    s.envScale = CVar("EnvScale", 1); s.envPanSpeed = CVar("EnvPanSpeed", 0.0005f);
    s.drawSky = CVar("DrawSky", 1); s.drawWorld = CVar("DrawWorld", 1); s.drawModels = CVar("DrawModels", 1);
    s.drawSprites = CVar("DrawSprites", 1); s.drawParticles = CVar("DrawParticles", 1); s.drawPolyGrids = CVar("DrawPolyGrids", 1);
    s.drawLineSystems = CVar("DrawLineSystems", 1); s.drawWorldModels = CVar("DrawWorldModels", 1);
    s.rt = CVar("lr_RT", 1); s.rtShadows = CVar("lr_RTShadows", 1); s.rtReflections = CVar("lr_RTReflections", 1);
    s.rtAO = CVar("lr_RTAO", 1); s.rtGI = CVar("lr_RTGI", 0);
    s.vsync = CVar("lr_VSync", 1); s.maxFps = CVar("lr_MaxFPS", 0); s.debugView = CVar("lr_DebugView", 0);
    s.exposure = CVar("lr_Exposure", 1); s.widescreen = CVar("lr_Widescreen", 1);
}

void Scene_Render(LT1SceneDesc *scene) {
    g_scene = scene;
    RenderContext *ctx = (RenderContext *)scene->m_pRenderContext;
    LT1MainWorld *world = ctx ? ctx->mainWorld : nullptr;
    bool haveWorld = world && world == g_world && !g_bsps.empty();

    float q[4] = {scene->m_Rotation.x, scene->m_Rotation.y, scene->m_Rotation.z, scene->m_Rotation.w};
    M34 cam = QuatToM(q);
    g_camFwd[0] = cam.m[0][2]; g_camFwd[1] = cam.m[1][2]; g_camFwd[2] = cam.m[2][2];
    g_camPos[0] = scene->m_Pos.x; g_camPos[1] = scene->m_Pos.y; g_camPos[2] = scene->m_Pos.z;

    // ---- polled world state: surface textures (animated textures swap Surface +0x48 without notice) and
    // texture coordinates (surface effects regenerate them in place)
    static std::vector<uint32_t> surfTex;
    static Out uv;
    surfTex.clear(); uv.d.clear();
    uint32_t nUvPolys = 0;
    if (haveWorld && scene->m_DrawMode == LT1_DRAWMODE_NORMAL) {
        size_t vi = 0; uint32_t polyIndex = 0;
        for (BspRef &b : g_bsps) {
            LT1WorldBsp *bsp = b.bsp;
            for (uint32_t s = 0; s < bsp->m_nSurfaces; s++) surfTex.push_back(TexId(bsp->m_Surfaces[s].m_pTexture));
            for (uint32_t p = 0; p < bsp->m_nPolies; p++) {
                LT1WorldPoly *poly = bsp->m_Polies[p];
                if (!poly || !poly->m_pSurface) continue;
                uint32_t n = poly->m_nVertices;
                bool changed = false;
                for (uint32_t v = 0; v < n; v++)
                    if (g_uvCache[(vi + v) * 2] != poly->m_Vertices[v].m_U || g_uvCache[(vi + v) * 2 + 1] != poly->m_Vertices[v].m_V) { changed = true; break; }
                if (changed) {
                    uv.put(lr::UvUpdate{polyIndex, n});
                    for (uint32_t v = 0; v < n; v++) {
                        float t[2] = {poly->m_Vertices[v].m_U, poly->m_Vertices[v].m_V};
                        g_uvCache[(vi + v) * 2] = t[0]; g_uvCache[(vi + v) * 2 + 1] = t[1];
                        uv.put(t);
                    }
                    nUvPolys++;
                }
                vi += n; polyIndex++;
            }
        }
    }

    // ---- objects
    g_obj.d.clear(); g_nObjects = 0; g_seen.clear();
    if (scene->m_DrawMode == LT1_DRAWMODE_OBJECTLIST) {
        if (scene->m_pObjectList)
            for (int i = 0; i < scene->m_ObjectListSize; i++) EmitObject(scene->m_pObjectList[i], lr::OBJ_VIEWLIST);
    } else if (haveWorld) {
        CollectWorldObjects(world->m_pWorldBsp);
        if (scene->m_SkyObjects)
            for (int i = 0; i < scene->m_nSkyObjects && i < 30; i++) EmitObject(scene->m_SkyObjects[i], lr::OBJ_SKY);
    }

    uint32_t extra = (uint32_t)surfTex.size() * 4 + uv.size() + g_obj.size();
    auto *c = bridge::Emit<lr::SceneCmd>(lr::CmdScene, extra);
    c->contextId = haveWorld ? g_contextId : 0;
    c->drawMode = scene->m_DrawMode; c->frameCode = scene->m_FrameCode; c->frameTime = scene->m_FrameTime;
    c->rect = lr::RectI{scene->m_Rect.left, scene->m_Rect.top, scene->m_Rect.right, scene->m_Rect.bottom};
    c->xFov = scene->m_xFov; c->yFov = scene->m_yFov; c->farZ = scene->m_FarZ;
    c->camPos = V(scene->m_Pos); c->camRot = lr::Quat{q[0], q[1], q[2], q[3]};
    c->modelAdd = V(scene->m_GlobalModelLightAdd); c->modelDirAdd = V(scene->m_GlobalModelDirAdd);
    c->lightScale = V(scene->m_GlobalLightScale); c->lightAdd = V(scene->m_GlobalLightAdd);
    c->skyMin = V(scene->m_SkyDef.m_Min); c->skyMax = V(scene->m_SkyDef.m_Max);
    c->skyViewMin = V(scene->m_SkyDef.m_ViewMin); c->skyViewMax = V(scene->m_SkyDef.m_ViewMax);
    for (int i = 0; i < 2; i++) {
        const LT1GlobalPanInfo &g = g_rs->m_GlobalPans[i];
        c->pans[i] = lr::GlobalPan{TexId(g.m_pTexture), g.m_xOffset, g.m_zOffset, g.m_xScale, g.m_zScale};
    }
    c->envMapTex = TexId(g_rs->m_pEnvMapTexture);
    c->nSurfaceTex = (uint32_t)surfTex.size(); c->nUvPolys = nUvPolys; c->uvBytes = uv.size();
    c->nObjects = g_nObjects; c->objectBytes = g_obj.size();
    FillSettings(c->settings);
    uint8_t *p = (uint8_t *)(c + 1);
    memcpy(p, surfTex.data(), surfTex.size() * 4); p += surfTex.size() * 4;
    memcpy(p, uv.d.data(), uv.size()); p += uv.size();
    memcpy(p, g_obj.d.data(), g_obj.size());
}

void *Scene_CreateContext(LT1RenderContextInit *init) {
    RenderContext *ctx = (RenderContext *)calloc(1, 0x430);     // both original renderers allocate 0x430
    ctx->mainWorld = init ? init->m_pMainWorld : nullptr;
    ctx->frameCode = 0xffff;
    g_contextId++;
    __try {
        SendWorld(ctx->mainWorld);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bridge::Log("CreateContext: exception %08lx while reading the world", GetExceptionCode());
        g_bsps.clear(); g_world = nullptr;
    }
    bridge::Flush();
    return ctx;         // must not be null: the engine softlocks on the loading screen otherwise
}

void Scene_DeleteContext(void *context) {
    if (context && ((RenderContext *)context)->mainWorld == g_world) {
        auto *c = bridge::Emit<lr::WorldEndCmd>(lr::CmdWorldEnd);
        c->contextId = g_contextId;
        g_world = nullptr; g_bsps.clear(); g_bspByWorldData.clear(); g_uvCache.clear();
        FreeModels();   // models are unloaded with the world and their addresses get reused
    }
    free(context);
}

bool Scene_HaveWorld() { return g_world != nullptr; }

void Scene_Reset() {
    g_world = nullptr; g_bsps.clear(); g_bspByWorldData.clear(); g_uvCache.clear();
    g_models.clear();   // the host drops everything on Term
}
