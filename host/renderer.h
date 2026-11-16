// The host renderer: owns every GPU resource, keeps the scene (world, textures, models, per-object history) and
// turns the frame's SceneCmd into a rasterised + ray-queried image. It never sees an engine pointer.
#pragma once
#include "gfx.h"
#include "mathx.h"
#include "../common/protocol.h"
#include <unordered_map>
#include <memory>

// ---- GPU layouts (mirror shaders/common.glsl, scalar block layout)
struct GVertex { float pos[3]; uint32_t color; float uv[2]; float lm[2]; float n[3]; uint32_t pad; };
struct GInstance {
    float r[3][4]; float p[3][4];
    uint32_t firstIndex, triBase, tex, flags;
    float color[4]; float light[4];
    int32_t prevVertexDelta; uint32_t pad[3];
};
struct GSurface { uint32_t tex, flags, pad0, pad1; };
struct GLight { float posRadius[4]; float color[4]; };
struct GTexInfo { uint32_t flags; float gloss; };
struct GFrame {
    M4 viewProj[3]; M4 prevViewProj[3];
    float camPos[4], fogColor[4], fogParams[4], lightScale[4], modelLightDir[4], rt[4], misc[4], skyColor[4], viewport[4];
    uint32_t nLights, pad[3];
};
static_assert(sizeof(GVertex) == 48 && sizeof(GInstance) == 160, "gpu layout");

enum : uint32_t { I_WORLD = 1, I_CONTAINER = 2, I_MODEL = 4, I_UNLIT = 8, I_ALPHATEST = 0x10, I_NOTEXTURE = 0x20, I_CHROME = 0x40,
                  I_WATER = 0x80, I_SKY = 0x100, I_CLOSE = 0x200 };
enum : uint32_t { T_FULLBRIGHT = 1, T_ALPHA = 2 };
constexpr uint32_t kMaxTextures = 4096, kTexLightmap = 4095;

enum Pipe { P_OPAQUE, P_SKYMASK, P_BLEND, P_ADD, P_NOZ, P_BLEND_ZWRITE, P_COUNT };
// d3d.ren's fixed queue order (d3d_FlushObjectQueues); no depth sorting inside a group.
enum Group { G_SKY, G_WORLD, G_SOLID_MODELS, G_SOLID_WORLDMODELS, G_SOLID_POLYGRIDS, G_TRANS_MODELS, G_PARTICLES, G_TRANS_POLYGRIDS,
             G_LINES, G_TRANS_WORLDMODELS, G_SPRITES, G_SPRITES_NOZ, G_CLOSE, G_COUNT };

struct Draw { uint32_t instance, firstIndex, indexCount; uint8_t pipe, view, group; };

struct ModelDef {
    std::vector<lr::ModelVertex> verts; std::vector<lr::ModelFace> faces; uint32_t nNodes = 0; std::string name;
};
struct BspGpu {
    uint32_t firstSurface = 0, nSurfaces = 0;
    uint32_t firstIndex = 0, nTris = 0, triBase = 0;            // opaque range
    uint32_t skyFirstIndex = 0, skyTris = 0, skyTriBase = 0;    // sky portal polys (depth only)
    bool translucent = false;
    Accel blas;
    V3 bmin, bmax;
};
struct CasterTri { uint32_t firstIndex, instance; };
struct History { M4 world; uint32_t firstVertex = 0, nVertices = 0; uint32_t frame = 0; };

class Renderer {
public:
    Gfx gfx;
    void init();
    void shutdown();

    // decode (called while the shim waits: must copy what it needs)
    void onInit(const lr::InitCmd &c);
    void onTerm();
    void onTexUpload(const lr::TexUploadCmd &c, const uint8_t *payload);
    void onTexFree(uint32_t id);
    void onWorldBegin(const lr::WorldBeginCmd &c, const uint8_t *payload);
    void onWorldEnd();
    void onModelDef(const lr::ModelDefCmd &c, const uint8_t *payload);
    void onModelFree(uint32_t id);
    void onScene(const lr::SceneCmd &c, uint32_t size);
    void onOverlay(const lr::OverlayCmd &c, const uint8_t *payload);
    void onScreenshot(const lr::ScreenshotCmd &c);
    void renderFrame(bool hadScene);

    float lastFrameMs = 0, lastGpuMs = 0;
    HWND gameWindow = nullptr, viewWindow = nullptr;

private:
    void createStatic();
    void createTargets();
    void destroyTargets();
    VkPipeline makeScenePipeline(Pipe p, bool rt);
    void buildScene(const lr::SceneCmd &sc);
    void addWorldDraws(const lr::SceneCmd &sc);
    void addObject(const lr::SceneCmd &sc, const lr::ObjectRecord &o);
    void addModel(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload);
    void addSprite(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const lr::SpritePayload &p);
    void addParticles(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload);
    void addPolyGrid(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload);
    void addLines(const lr::SceneCmd &sc, const lr::ObjectRecord &o, const uint8_t *payload);
    uint32_t pushInstance(const GInstance &i);
    void pushDraw(Group g, Pipe p, uint32_t instance, uint32_t firstIndex, uint32_t indexCount, uint8_t view = 0);
    GVertex *allocDyn(uint32_t nVerts, uint32_t &firstVertex);
    uint32_t *allocDynIndices(uint32_t n, uint32_t &firstIndex);
    V3 sampleGrid(V3 pos) const;
    void buildAccel(VkCommandBuffer cmd);
    void writeTextureSlot(uint32_t slot, VkImageView view);
    void saveScreenshot();
    bool ensureWindow();

    // device objects
    VkDescriptorSetLayout sceneLayout = VK_NULL_HANDLE, postLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet sceneSet = VK_NULL_HANDLE, postSet[2] = {};
    VkPipelineLayout scenePL = VK_NULL_HANDLE, postPL = VK_NULL_HANDLE;
    VkPipeline pipes[2][P_COUNT] = {}, postPipe = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, clampSampler = VK_NULL_HANDLE, nearestSampler = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquireSem = VK_NULL_HANDLE, renderSem = VK_NULL_HANDLE;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    bool frameInFlight = false;

    Image sceneColor, sceneAux, sceneDepth, history[2], overlayImg, white;
    Image textures[kMaxTextures];
    std::vector<GTexInfo> texInfoCpu;
    Buffer vbuf, ibuf, instBuf, triBuf, surfBuf, lightBuf, texInfoBuf, frameBuf, tlasInstBuf, scratch, shotBuf;
    Accel tlas, dynBlas;

    // world
    bool haveWorld = false;
    uint32_t worldContext = 0;
    std::vector<BspGpu> bsps;
    std::vector<GSurface> surfaces;
    std::vector<uint32_t> polyFirstVertex, polyVertexCount;
    uint32_t staticVerts = 0, staticIndices = 0, staticTris = 0;
    lr::LightGrid grid = {};
    std::vector<uint8_t> gridData;
    std::string worldName;

    std::unordered_map<uint32_t, ModelDef> models;
    std::unordered_map<uint32_t, History> historyMap;

    // frame
    std::vector<uint8_t> sceneCopy;         // the frame's main SceneCmd (first normal-mode scene)
    std::vector<std::vector<uint8_t>> extraScenes;
    bool sceneValid = false;
    std::vector<GInstance> instances;
    std::vector<Draw> draws;
    std::vector<GLight> lights;
    std::vector<VkAccelerationStructureInstanceKHR> tlasInstances;
    std::vector<CasterTri> casterTris;
    uint32_t dynHalf = 0, dynVerts = 0, dynIndices = 0, dynTris = 0;
    uint32_t rtDynFirstIndex = 0, rtDynTris = 0;      // dynamic BLAS range (opaque casters come first)
    uint32_t frameIndex = 0;
    GFrame frameData = {};
    M4 prevViewProj[3]; bool prevValid = false;
    V3 camPos, camRight, camUp, camFwd, skyEye;
    float tanX = 1, tanY = 1, nearZ = 7, farZ = 15000;
    VkViewport sceneViewport = {};
    lr::Settings settings = {};
    float lightAdd[3] = {};
    bool historyValid = false;

    // 2D
    uint32_t modeW = 640, modeH = 480;
    bool windowed = false, overlayEmpty = true;
    std::string shotPath; uint32_t shotFormat = 0;
    uint64_t statFrames = 0; double statMs = 0; LARGE_INTEGER statStart = {};
};
