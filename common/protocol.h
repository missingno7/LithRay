// LithRay bridge protocol: the only contract between the 32-bit shim (lithray.ren, inside CLIENT.EXE) and the
// 64-bit host (lithray_host.exe). Everything here is plain data with fixed-width fields: no pointers, no engine
// types. Both sides are built from this one header; bump kVersion on any layout change.
//
// Transport (docs/ARCHITECTURE.md): one named file mapping = SharedHeader + a linear command arena. The shim
// appends commands, then "flushes": sets header.used, signals evSubmit and waits for evDone. The host decodes
// (copies everything it needs), signals evDone, and only then renders. The arena is reused from offset 0.
#pragma once
#include <stdint.h>

namespace lr {

constexpr uint32_t kMagic = 0x5941524c;      // 'LRAY'
constexpr uint32_t kVersion = 9;
constexpr uint32_t kHeaderSize = 4096;
constexpr uint32_t kArenaSize = 64u << 20;

enum HostState : uint32_t { HostStarting = 0, HostReady = 1, HostFailed = 2, HostExited = 3 };

struct SharedHeader {
    uint32_t magic, version;
    uint32_t arenaSize;
    uint32_t gamePid, hostPid;
    volatile uint32_t hostState;        // HostState, written by the host
    volatile uint32_t used;             // bytes of commands in the arena, written by the shim before evSubmit
    volatile uint32_t flushCount;       // shim increments per flush
    volatile uint32_t ackCount;         // host sets = flushCount when decoded
    // host -> shim status (read after a flush)
    uint32_t swapWidth, swapHeight;     // presentation size in pixels
    uint32_t rtSupported;               // hardware ray tracing available on the chosen device
    float    hostFrameMs, hostGpuMs;    // last rendered frame, for the shim's log / console
    char     deviceName[128];
    char     hostMessage[256];          // last error or status line
};
static_assert(sizeof(SharedHeader) <= kHeaderSize, "header");

// Object names: L"Local\\LithRay.<gamePid>.map|submit|done"
#define LITHRAY_OBJECT_FMT L"Local\\LithRay.%u.%s"

enum Cmd : uint32_t {
    CmdInit = 1, CmdTerm, CmdTexUpload, CmdTexFree, CmdWorldBegin, CmdWorldEnd, CmdModelDef, CmdModelFree,
    CmdScene, CmdOverlay, CmdPresent, CmdScreenshot, CmdConfig,
};

struct CmdHeader { uint32_t type, size; };   // size = whole command including this header, multiple of 8

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
struct RectI { int32_t left, top, right, bottom; };

// ---- lifetime
struct InitCmd {
    uint64_t hwnd;                  // the game's top-level window
    uint32_t modeWidth, modeHeight; // the engine's 2D coordinate space
    uint32_t windowed;
    uint32_t pad;
};

// ---- textures. id is shim-issued, non-zero, reused after TexFree.
// Payload after the struct: palette[256][4] as (a, r, g, b); then per mip: indices[w*h] (8-bit), and when
// (dtxFlags & DTX_ALPHAMASKS): alpha[(w*h+1)/2] (4-bit, low nibble = even texel). Mip n is (w>>n, h>>n), min 1.
enum : uint32_t { DTX_FULLBRITE = 1, DTX_ALPHAMASKS = 2 };
struct TexUploadCmd {
    uint32_t id, width, height, nMips, dtxFlags, pad;
    char     name[96];              // engine file name, for diagnostics and material overrides
};
struct TexFreeCmd { uint32_t id, pad; };

// ---- world. One WorldBegin per CreateContext. All BSPs of the level are concatenated: bsp 0 is the main world,
// bsp k (k >= 1) is world model k-1 in its ORIGINAL (untransformed) space; instances place them per frame.
enum : uint32_t { SURF_SOLID = 1, SURF_NONEXISTANT = 2, SURF_INVISIBLE = 4, SURF_TRANSPARENT = 8, SURF_SKY = 0x10,
                  SURF_BRIGHT = 0x20, SURF_GOURAUD = 0x40, SURF_LIGHTMAP = 0x80, SURF_PANNINGSKY = 0x8000 };
struct WorldBsp { uint32_t firstSurface, nSurfaces, firstPoly, nPolys; uint32_t key; uint32_t pad; };
struct WorldSurface { uint32_t flags; Vec3 normal; };
struct WorldPoly {
    uint32_t surface;               // global surface index
    uint32_t firstVertex, nVertices;// fan
    uint32_t lmOffset;              // byte offset into the lightmap blob (w*h RGB565), 0xffffffff = none
    uint16_t lmWidth, lmHeight;
    uint32_t pad;
};
struct WorldVertex { Vec3 pos; float u, v; float lmu, lmv; uint8_t color[4]; };  // uv in texels; lm uv in lightmap texels; color b,g,r,a
struct LightGrid { int32_t dims[3]; Vec3 blockSize, lookupStart; uint32_t nSamples; };  // samples: b, g, r, pad
struct WorldBeginCmd {
    uint32_t contextId;
    uint32_t nBsps, nSurfaces, nPolys, nVertices, lightmapBytes;
    LightGrid grid;
    char     worldName[100];
    uint32_t pad;
    Vec3     worldMin, worldMax;    // the main BSP's box (WorldBsp +0xe0): the sky camera is placed by the view's fraction through it
    char     worldFile[128];        // the level file the server loaded ("Worlds\x.dat"), "" when unknown
    char     rezSources[512];       // the game's -rez sources in command-line order, ';' separated (later overrides earlier)
    // then: WorldBsp[nBsps], WorldSurface[nSurfaces], WorldPoly[nPolys], WorldVertex[nVertices],
    //       uint8 lightmaps[lightmapBytes] (padded to 8), uint8 grid[nSamples*4]
};
struct WorldEndCmd { uint32_t contextId, pad; };

// ---- model geometry (immutable per id). Sent before the first object that uses it.
struct ModelVertex { Vec3 pos; int8_t normal[3]; uint8_t node; };
struct ModelFace { uint16_t v[3]; uint16_t nodeSelector; float uv[6]; };   // uv in texels of the skin
struct ModelDefCmd {
    uint32_t id, nVertices, nFaces, nNodes;
    char     name[96];
    // then ModelVertex[nVertices], ModelFace[nFaces]
};
struct ModelFreeCmd { uint32_t id, pad; };

// ---- per-frame scene
enum ObjType : uint8_t { OT_NORMAL, OT_MODEL, OT_WORLDMODEL, OT_SPRITE, OT_LIGHT, OT_CAMERA, OT_PARTICLESYSTEM,
                         OT_POLYGRID, OT_LINESYSTEM, OT_CONTAINER };
enum : uint32_t {   // engine object flags (released basetypes_de.h); several bits are reused per object type
    FLAG_VISIBLE = 1 << 0,
    FLAG_SHADOW = 1 << 1, FLAG_UNSIGNED = 1 << 1,               // model casts shadows / polygrid data is unsigned
    FLAG_MODELTINT = 1 << 2,
    FLAG_CASTSHADOWS = 1 << 3, FLAG_ROTATEABLESPRITE = 1 << 3, FLAG_MODELGOURAUDSHADE = 1 << 3, FLAG_UPDATEUNSEEN = 1 << 3,
    FLAG_SOLIDLIGHT = 1 << 4, FLAG_SPRITECHROMAKEY = 1 << 4, FLAG_MODELWIREFRAME = 1 << 4, FLAG_WASDRAWN = 1 << 4,
    FLAG_GLOWSPRITE = 1 << 5, FLAG_ONLYLIGHTWORLD = 1 << 5, FLAG_ENVIRONMENTMAP = 1 << 5, FLAG_ENVIRONMENTMAPONLY = 1 << 5,
    FLAG_SPRITEBIAS = 1 << 6, FLAG_DONTLIGHTBACKFACING = 1 << 6, FLAG_REALLYCLOSE = 1 << 6,
    FLAG_FOGLIGHT = 1 << 7, FLAG_ANIMTRANSITION = 1 << 7, FLAG_SPRITE_NOZ = 1 << 7,
    FLAG_NOLIGHT = 1 << 9, FLAG_SKYOBJECT = 1 << 12,
};
enum : uint32_t { MHF_USETEXTURE = 1 };
enum : uint8_t { OBJ_SKY = 1, OBJ_VIEWLIST = 2, OBJ_ATTACHMENT = 4 };

struct ObjectRecord {           // followed by a per-type payload; size covers both, multiple of 8
    uint32_t size;
    uint8_t  type, role, pad[2];
    uint32_t key;               // engine object address: identity hint only (slots are reused)
    uint32_t flags, userFlags;
    uint8_t  rgba[4];
    Vec3     pos; Quat rot; Vec3 scale;
    float    radius;
};
struct ModelPayload {
    uint32_t modelId, skinTex;
    uint32_t hookFlags;         // MHF_* after the game's model hook
    Vec3     lightAdd;          // 0..255, after the model hook
    uint32_t nNodes;            // then float[nNodes][12]: node -> object-local 3x4 row-major (no object matrix)
    uint32_t nDeform;           // then Vec3[nDeform]: this frame's base vertex positions (0 = use ModelDef)
    uint32_t nHidden;           // then uint8[nHidden] hidden node selectors (padded to 4)
    uint32_t pad;
};
struct WorldModelPayload { uint32_t bsp; uint32_t pad; float transform[16]; };  // row-major local -> world
// nClip > 0: the sprite is a decal clipped to a world polygon; then Vec3 planeNormal, float planeDist, Vec3[nClip] (world space)
struct SpritePayload { uint32_t tex; uint32_t nClip; };
struct ParticlePayload { uint32_t tex, count; float particleRadius; uint32_t psFlags; };   // then {Vec3 pos; uint8 rgba[4]}[count], pos system-local
struct Particle { Vec3 pos; uint8_t rgba[4]; };
struct PolyGridPayload {
    uint32_t tex, envTex; uint32_t width, height; float xPan, yPan, xScale, yScale; uint32_t nIndices, pad;
    // then int8 heights[width*height] (pad 4), float colors[256][4], uint16 indices[nIndices] (pad 4)
};
struct LinePayload { uint32_t count, pad; };   // then {Vec3 a; float ca[4]; Vec3 b; float cb[4]}[count]
struct LineSeg { Vec3 a; float ca[4]; Vec3 b; float cb[4]; };
struct LightPayload { float radius; uint32_t pad; };

struct GlobalPan { uint32_t tex; float xOffset, zOffset, xScale, zScale; };

// Console state polled by the shim once per frame (engine variables first, LithRay options after).
struct Settings {
    float fogEnable, fogNearZ, fogFarZ, fogR, fogG, fogB, skyFogNearZ, skyFogFarZ, skyScale;
    float saturate, lightSaturate, lightModels, dynamicLight, envMapEnable, envMapAll, envScale, envPanSpeed;
    float drawSky, drawWorld, drawModels, drawSprites, drawParticles, drawPolyGrids, drawLineSystems, drawWorldModels;
    // lr_* options
    float rt;               // 0 raster baseline, 1 ray-traced lighting
    float rtShadows, rtReflections, rtAO, rtGI;
    float vsync, maxFps, debugView, exposure, widescreen;
    float giDirect, giIndirect, giHistory;      // full ray-traced lighting: lamp scale, bounce scale, history weight
    float giEmissive;                           // strength of light emitted by bright surfaces and fullbright texels
    float reserved[8];
};

struct SceneCmd {
    uint32_t contextId;
    uint32_t drawMode;          // 1 normal, 2 object list
    uint32_t frameCode;
    float    frameTime;
    RectI    rect;              // in mode coordinates
    float    xFov, yFov, farZ;
    Vec3     camPos; Quat camRot;
    Vec3     modelAdd, modelDirAdd, lightScale, lightAdd;
    Vec3     skyMin, skyMax, skyViewMin, skyViewMax;
    GlobalPan pans[2];
    uint32_t envMapTex;
    uint32_t nSurfaceTex;       // then uint32 tex id per world surface (all BSPs, WorldBegin order)
    uint32_t nUvPolys;          // then UvUpdate records (animated surfaces): {poly, nVertices} + float[nVertices][2]
    uint32_t uvBytes;
    uint32_t nObjects, objectBytes; // then ObjectRecord stream
    uint32_t pad;
    Settings settings;
};
struct UvUpdate { uint32_t poly, nVertices; };

// ---- 2D layer: the engine's 16-bit back buffer as composed by the shim. Sent only when it changed.
// Payload: uint16 pixels[w*h] (RGB565), uint8 mask[w*h] (255 where 2D was drawn since the last clear).
struct OverlayCmd { uint32_t width, height; uint32_t empty; uint32_t pad; };

struct PresentCmd { uint32_t hadScene; uint32_t pad; };
struct ScreenshotCmd { char path[260]; uint32_t format; };  // 0 = BMP (engine F8), 1 = PNG (capture)

inline uint32_t Align8(uint32_t n) { return (n + 7u) & ~7u; }

} // namespace lr
