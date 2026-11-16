// lt1_port_abi.h -- the Blood II 2.1 LT1 renderer ABI as one self-contained header, with compile-time offset checks.
//
// RECONSTRUCTED / OURS: a port reference for renderer DLLs (d_ren, the SDL3 port). It is not compiled into any
// strict-verified unit. docs/renderer_closure.md (section A) and analysis/renderer_knowledge.json give the
// per-field evidence class; docs/seed_pass/renderer_port_abi.md is the older per-field table they correct. Names
// are ours unless RELEASED SDK names exist. Only fields a renderer reads or writes are named; the rest is padding
// of the proven size. Pointers are 32-bit: this header describes the legacy 32-bit plugin boundary only, and a
// native x64 frontend must declare its own structures (docs/renderer_modern_interface.md).
#ifndef LT1_PORT_ABI_H
#define LT1_PORT_ABI_H

#include <stddef.h>

#define LT1P_CHECK(type, member, off)   typedef char lt1p_off_##type##_##member[(offsetof(type, member) == (off)) ? 1 : -1];
#define LT1P_SIZE(type, size)           typedef char lt1p_size_##type[(sizeof(type) == (size)) ? 1 : -1];

typedef unsigned int    lt1_u32;
typedef unsigned short  lt1_u16;
typedef unsigned char   lt1_u8;

struct LT1Vector   { float x, y, z; };
struct LT1Rotation { float x, y, z, w; };
struct LT1Matrix   { float m[4][4]; };               // row-major, translation in m[0..2][3]
struct LT1Plane    { LT1Vector m_Normal; float m_Dist; };
struct LT1Rect     { int left, top, right, bottom; };
struct LT1Link     { LT1Link *m_pPrev, *m_pNext; void *m_pData; };
struct LT1List     { lt1_u32 m_nElements; LT1Link m_Head; };

struct LT1SharedTexture;
struct LT1DEPalette;
struct LT1MainWorld;
struct LT1DObject;

// ---------------------------------------------------------------- interface
// RELEASED basedefs_de.h RMode_t (DBOOL = char). NATIVE: d3d.ren 0x15d36 `mov byte ptr [ebx],1` (one byte),
// 0x15d46/0x15d65 copy the names to +0xc9/+0x12d; Client.exe 0x452b7 `lea esi,[ebp+1]` -> LoadLibrary.
// Corrected 2026-10-12: an earlier revision declared a 4-byte m_bHardware with strings at +4/+0xcc/+0x130;
// the total size (0x1a4) and the width fields (+0x194, alignment padding) are identical under both readings.
struct LT1RMode
{
    char            m_bHardware;            // +0x000 DBOOL
    char            m_RenderDLL[200];       // +0x001 written by the engine (file name of the .ren)
    char            m_InternalName[100];    // +0x0c9
    char            m_Description[100];     // +0x12d
    lt1_u32         m_Width, m_Height, m_BitDepth;  // +0x194 (3 bytes of padding before)
    LT1RMode        *m_pNext;               // +0x1a0
};
LT1P_CHECK(LT1RMode, m_RenderDLL, 0x001)
LT1P_CHECK(LT1RMode, m_InternalName, 0x0c9)
LT1P_CHECK(LT1RMode, m_Description, 0x12d)
LT1P_CHECK(LT1RMode, m_Width, 0x194)
LT1P_CHECK(LT1RMode, m_pNext, 0x1a0)
LT1P_SIZE(LT1RMode, 0x1a4)

struct LT1RenderStructInit
{
    LT1RMode        m_Mode;                 // +0x000
    void            *m_hWnd;                // +0x1a4 HWND
};
LT1P_CHECK(LT1RenderStructInit, m_hWnd, 0x1a4)

struct LT1BlitRequest
{
    void            *m_hBuffer;             // +0x00 the renderer's surface
    lt1_u32         m_BlitOptions;          // +0x04 bit 0: transparent
    lt1_u32         m_TransparentColor;     // +0x08
    LT1Rect         *m_pSrcRect;            // +0x0c
    LT1Rect         *m_pDestRect;           // +0x10
};
LT1P_SIZE(LT1BlitRequest, 0x14)

// Exactly one pointer: STRICT CClientShell::BindWorlds (Client 0x115a0) passes the address of one local; STRICT
// d3d_CreateContext (0x1d1a0) reads [init+0] only.
struct LT1RenderContextInit { LT1MainWorld *m_pMainWorld; };
LT1P_SIZE(LT1RenderContextInit, 4)

struct LT1GlobalPanInfo
{
    LT1SharedTexture *m_pTexture;           // +0x00
    float           m_xOffset, m_zOffset;   // +0x04
    float           m_xScale, m_zScale;     // +0x0c
};
LT1P_SIZE(LT1GlobalPanInfo, 0x14)

struct LT1RenderStruct
{
    // Engine callbacks (r_InitRenderStruct, Client.exe 0x44e50).
    void            *(*GetAttachmentObject)(void *pParent, void *pAttachment);     // +0x00
    void            *(*GetTexture)(LT1SharedTexture *pTexture, void *pOut);       // +0x04
    void            (*FreeTexture)(LT1SharedTexture *pTexture);                   // +0x08
    LT1DEPalette    *(*DLinkToPalette)(LT1Link *pLink);                           // +0x0c
    void            *(*GetPaletteColours)(LT1DEPalette *pPalette);                // +0x10
    void            *(*GetPaletteRenderData)(LT1DEPalette *pPalette);             // +0x14
    void            (*SetPaletteRenderData)(LT1DEPalette *pPalette, void *pData); // +0x18
    void            (*RunConsoleString)(char *pStr);                              // +0x1c
    void            (*CPrint)(char *pMsg, ...);                                   // +0x20
    void            *(*GetConsoleVar)(char *pName);                               // +0x24
    float           (*GetVarValueFloat)(void *hVar);                              // +0x28
    char            *(*GetVarValueString)(void *hVar);                            // +0x2c
    void            (*UnknownFunc_5)();     // +0x30 empty in release (STRICT Client 0x4480, folded); d3d.ren calls
                                            // it 9 times per RenderScene, once per object group (BEHAVIORAL trace)
    lt1_u32         m_Width, m_Height;      // +0x34 engine-set from RMode; d3d.ren Init divides m_Width by its scale
    int             m_bInitted;             // +0x3c engine; gates every slot call
    lt1_u32         m_SceneTextureMemory, m_SceneLightmapMemory;    // +0x40 renderer-written, engine-displayed
    lt1_u32         m_TextureMemory, m_MPTextureMemorySaved;        // +0x48 renderer-written
    void            *m_Slots[34];           // +0x50 .. +0xd4 renderer slots (docs table)
    lt1_u32         m_UnknownD8;            // +0xd8
    LT1SharedTexture *m_pEnvMapTexture;     // +0xdc
    LT1GlobalPanInfo m_GlobalPans[2];       // +0xe0 GLOBALPAN_SKYSHADOW, GLOBALPAN_FOGLAYER
    void            *m_hRenderDLL;          // +0x108
    void            *m_PaletteList;         // +0x10c
};
LT1P_CHECK(LT1RenderStruct, UnknownFunc_5, 0x30)
LT1P_CHECK(LT1RenderStruct, m_Width, 0x34)
LT1P_CHECK(LT1RenderStruct, m_TextureMemory, 0x48)
LT1P_CHECK(LT1RenderStruct, m_Slots, 0x50)
LT1P_CHECK(LT1RenderStruct, m_UnknownD8, 0xd8)
LT1P_CHECK(LT1RenderStruct, m_GlobalPans, 0xe0)
LT1P_CHECK(LT1RenderStruct, m_hRenderDLL, 0x108)
LT1P_SIZE(LT1RenderStruct, 0x110)

// Renderer slot indices into m_Slots (offset = 0x50 + 4 * index).
enum LT1RenderSlot
{
    LT1RS_Init, LT1RS_Term, LT1RS_SetSoftSky, LT1RS_BindTexture, LT1RS_UnbindTexture, LT1RS_QueryDeletePalette,
    LT1RS_SetMasterPalette, LT1RS_CreateContext, LT1RS_DeleteContext, LT1RS_Clear, LT1RS_Start3D, LT1RS_End3D,
    LT1RS_IsIn3D, LT1RS_StartOptimized2D, LT1RS_EndOptimized2D, LT1RS_IsInOptimized2D, LT1RS_RenderScene,
    LT1RS_RenderCommand, LT1RS_GetHook, LT1RS_SwapBuffers, LT1RS_GetInfoFlags, LT1RS_GetBufferFormat,
    LT1RS_CreateSurface, LT1RS_DeleteSurface, LT1RS_GetSurfaceInfo, LT1RS_LockSurface, LT1RS_UnlockSurface,
    LT1RS_OptimizeSurface, LT1RS_UnoptimizeSurface, LT1RS_LockScreen, LT1RS_UnlockScreen, LT1RS_BlitToScreen,
    LT1RS_MakeScreenShot, LT1RS_ReadConsoleVariables, LT1RS_Count
};
typedef char lt1p_slotcount[(LT1RS_Count == 34) ? 1 : -1];

struct LT1SkyDef { LT1Vector m_Min, m_Max, m_ViewMin, m_ViewMax; };

struct LT1ModelHookData
{
    LT1DObject      *m_hObject;             // +0x00
    lt1_u32         m_Flags;                // +0x04 MHF_*
    lt1_u32         m_ObjectFlags;          // +0x08
    LT1Vector       *m_LightAdd;            // +0x0c
};

#define LT1_DRAWMODE_NORMAL      1
#define LT1_DRAWMODE_OBJECTLIST  2

struct LT1SceneDesc
{
    int             m_DrawMode;             // +0x00
    lt1_u32         *m_pTicks[6];           // +0x04 Objects, Models, Sprites, WorldModels, Particles, Lighting
                                            // (NATIVE Client 0xfd4d..0xfd75 + the "ctik" labels; +0x18 is
                                            // Lighting, not PolyGrids as ClientMgr.cpp:146 names it)
    LT1Vector       m_GlobalModelLightAdd;  // +0x1c console "ModelAdd"
    LT1Vector       m_GlobalModelDirAdd;    // +0x28 console "ModelDirAdd"
    void            *m_pRenderContext;      // +0x34 MainWorld +0xc8
    LT1Vector       m_GlobalLightColor;     // +0x38 copied but never read by d3d.ren
    LT1Vector       m_GlobalLightDir;       // +0x44 copied but never read by d3d.ren
    LT1Vector       m_GlobalLightScale;     // +0x50
    LT1Vector       m_GlobalLightAdd;       // +0x5c camera light add
    float           m_FrameTime;            // +0x68
    lt1_u16         m_FrameCode;            // +0x6c u16 on both sides (NATIVE d3d 0x138c5 movzx word); the
                                            // renderer stamps collected objects' DObject +0x6c with it
    lt1_u16         m_Pad6E;                // never written by the engine (stack garbage): do not read
    LT1SkyDef       m_SkyDef;               // +0x70
    LT1DObject      **m_SkyObjects;         // +0xa0 30-entry array outside the SceneDesc
    int             m_nSkyObjects;          // +0xa4
    LT1Rect         m_Rect;                 // +0xa8
    float           m_xFov, m_yFov;         // +0xb8
    float           m_FarZ;                 // +0xc0
    LT1Vector       m_Pos;                  // +0xc4
    LT1Rotation     m_Rotation;             // +0xd0
    LT1DObject      **m_pObjectList;        // +0xe0
    int             m_ObjectListSize;       // +0xe4
    void            (*m_ModelHookFn)(LT1ModelHookData *pData, void *pUser);    // +0xe8
    void            *m_ModelHookUser;       // +0xec
};
LT1P_CHECK(LT1SceneDesc, m_GlobalModelLightAdd, 0x1c)
LT1P_CHECK(LT1SceneDesc, m_pRenderContext, 0x34)
LT1P_CHECK(LT1SceneDesc, m_GlobalLightScale, 0x50)
LT1P_CHECK(LT1SceneDesc, m_FrameTime, 0x68)
LT1P_CHECK(LT1SceneDesc, m_FrameCode, 0x6c)
LT1P_CHECK(LT1SceneDesc, m_SkyDef, 0x70)
LT1P_CHECK(LT1SceneDesc, m_SkyObjects, 0xa0)
LT1P_CHECK(LT1SceneDesc, m_Rect, 0xa8)
LT1P_CHECK(LT1SceneDesc, m_Pos, 0xc4)
LT1P_CHECK(LT1SceneDesc, m_Rotation, 0xd0)
LT1P_CHECK(LT1SceneDesc, m_pObjectList, 0xe0)
LT1P_CHECK(LT1SceneDesc, m_ModelHookFn, 0xe8)
LT1P_SIZE(LT1SceneDesc, 0xf0)

// ---------------------------------------------------------------- world
struct LT1Surface;
struct LT1WorldPoly;
struct LT1Leaf;
struct LT1WorldBsp;

struct LT1SPolyVertex
{
    LT1Vector       *m_pPoint;              // +0x00
    float           m_U, m_V;               // +0x04 texels
    float           m_LMU, m_LMV;           // +0x0c renderer-written
    lt1_u8          m_Color[4];             // +0x14 b, g, r, a
};
LT1P_SIZE(LT1SPolyVertex, 0x18)

struct LT1Surface
{
    LT1Vector       O, P, Q;                // +0x00
    LT1Vector       m_LMVecs[3];            // +0x24
    LT1SharedTexture *m_pTexture;           // +0x48
    LT1Plane        *m_pPlane;              // +0x4c
    lt1_u32         m_Flags;                // +0x50 SURF_*
    lt1_u16         m_TextureFlags;         // +0x54
    lt1_u16         m_iTexture;             // +0x56
    lt1_u16         m_iFirstPoly;           // +0x58
    lt1_u8          m_bNoFastNodes, m_Unknown5B;
};
LT1P_CHECK(LT1Surface, m_pTexture, 0x48)
LT1P_CHECK(LT1Surface, m_Flags, 0x50)
LT1P_SIZE(LT1Surface, 0x5c)

struct LT1WorldPoly
{
    LT1Vector       m_Center;               // +0x00
    float           m_Radius;               // +0x0c
    LT1Surface      *m_pSurface;            // +0x10
    lt1_u32         m_Unknown14;
    void            *m_pDynamicLights;      // +0x18 renderer chain {next, light}
    lt1_u32         m_Unknown1C;
    LT1Vector       m_LMOrigin;             // +0x20
    void            *m_pLightmapPage;       // +0x2c renderer
    lt1_u8          m_LMX, m_LMY, m_LMWidth, m_LMHeight;   // +0x30 renderer
    lt1_u8          *m_pLightmap;           // +0x34 w, h, then w*h RGB565 words
    lt1_u16         m_iNextPoly;            // +0x38
    lt1_u16         m_Unknown3A;
    lt1_u32         m_LightmapPaged;        // +0x3c renderer
    lt1_u16         m_nVertices;            // +0x40
    lt1_u16         m_nExtraVertices;       // +0x42
    LT1SPolyVertex  m_Vertices[3];          // +0x44 (m_nVertices + m_nExtraVertices)
};
LT1P_CHECK(LT1WorldPoly, m_pSurface, 0x10)
LT1P_CHECK(LT1WorldPoly, m_pDynamicLights, 0x18)
LT1P_CHECK(LT1WorldPoly, m_pLightmap, 0x34)
LT1P_CHECK(LT1WorldPoly, m_nVertices, 0x40)
LT1P_CHECK(LT1WorldPoly, m_Vertices, 0x44)

struct LT1Node
{
    lt1_u32         m_Flags;                // +0x00 bits 0/1 = the IN/OUT sentinels (test `flags & 3`; which bit is
                                            // IN is OPEN: engine reconstruction IN=1/OUT=2, d_ren the reverse)
    LT1WorldPoly    *m_pPoly;               // +0x04
    LT1Plane        *m_pPlane;              // +0x08
    lt1_u16         m_PlaneType, m_Unknown0E;   // +0x0c
    LT1Leaf         *m_pLeaf;               // +0x10
    LT1WorldBsp     *m_pWorldBsp;           // +0x14
    LT1Vector       m_Center;               // +0x18
    float           m_Radius;               // +0x24
    void            *m_pFastNode;           // +0x28
    LT1Node         *m_Sides[2];            // +0x2c [0] back, [1] front
};
LT1P_SIZE(LT1Node, 0x34)

struct LT1LeafList { lt1_u16 m_PortalID, m_ListSize; lt1_u8 *m_pList; };

struct LT1Leaf
{
    LT1Vector       m_Center;               // +0x00
    float           m_Radius;               // +0x0c
    LT1LeafList     *m_LeafLists;           // +0x10
    LT1Link         m_LeafLinks;            // +0x14 objects in the leaf
    LT1WorldPoly    **m_Polies;             // +0x20
    lt1_u16         m_nPolies, m_Unknown26; // +0x24
    lt1_u32         m_Unknown28;
    lt1_u16         m_nLeafLists, m_Unknown2E;  // +0x2c
};
LT1P_SIZE(LT1Leaf, 0x30)

struct LT1WorldBsp
{
    lt1_u32         m_MemoryUse;            // +0x000
    lt1_u32         m_ClientDataPos;        // +0x004
    LT1Plane        *m_Planes;              // +0x008
    lt1_u32         m_nPlanes;
    LT1Node         *m_Nodes;               // +0x010
    lt1_u32         m_nNodes;
    void            *m_FastNodes;           // +0x018
    lt1_u32         m_nFastNodes;
    LT1Surface      *m_Surfaces;            // +0x020
    lt1_u32         m_nSurfaces;
    LT1LeafList     *m_LeafLists;           // +0x028
    lt1_u32         m_nLeafLists;
    LT1Leaf         *m_Leafs;               // +0x030
    lt1_u32         m_nLeafs;
    LT1WorldPoly    **m_LeafPolies;         // +0x038
    lt1_u32         m_nLeafPolies;
    lt1_u32         m_LeafListDataSize;     // +0x040
    LT1Node         *m_RootNode;            // +0x044
    void            *m_FastRoot;            // +0x048
    LT1WorldPoly    **m_Polies;             // +0x04c
    lt1_u32         m_nPolies;              // +0x050
    float           *m_Points;              // +0x054 4 floats each
    lt1_u32         m_nPoints;
    void            *m_Portals;             // +0x05c 0x24 each
    lt1_u32         m_nPortals;
    char            *m_TextureNameData;     // +0x064
    char            **m_TextureNames;       // +0x068
    lt1_u32         m_nTextures;            // +0x06c
    char            m_WorldName[100];       // +0x070
    lt1_u32         m_Unknown0D4;
    LT1DObject      *m_WorldObjects[2];     // +0x0d8
    LT1Vector       m_MinBox, m_MaxBox;     // +0x0e0
    LT1Vector       m_PosMin, m_PosMax;     // +0x0f8
    lt1_u32         m_WorldInfoFlags;       // +0x110
    lt1_u32         m_Unknown114;
    lt1_u32         m_bLightmapped;         // +0x118
    LT1Vector       m_WorldTranslation;     // +0x11c
    lt1_u8          *m_pLightmapData;       // +0x128
    lt1_u16         m_Index, m_Unknown12E;  // +0x12c 0xffff = main world
    lt1_u8          *m_PolyData;            // +0x130
    lt1_u8          *m_LeafListData;        // +0x134
    lt1_u8          m_PBlockTable[0x30];    // +0x138
};
LT1P_CHECK(LT1WorldBsp, m_RootNode, 0x44)
LT1P_CHECK(LT1WorldBsp, m_Polies, 0x4c)
LT1P_CHECK(LT1WorldBsp, m_WorldName, 0x70)
LT1P_CHECK(LT1WorldBsp, m_WorldTranslation, 0x11c)
LT1P_SIZE(LT1WorldBsp, 0x168)

// +0x00 = the transformed (world-space) BSP, +0x04 = the original (STRICT w_TransformWorldModel Server 0x7b71/0x7b74
// reads [wd+4] and writes [wd+0]). d3d.ren draws world models from +0x04 through the instance m_Transform; sprite
// clipper polys resolve through +0x00. servermgr_lt1.h:438 names +0x00 "m_pOriginalBsp", which is wrong.
struct LT1WorldData { LT1WorldBsp *m_pWorldBsp, *m_pOriginalBsp; LT1Link m_Link; };
LT1P_CHECK(LT1WorldData, m_pOriginalBsp, 0x04)
LT1P_SIZE(LT1WorldData, 0x14)

// The objects of a BSP region (WorldBsp +0x18 array, +0x1c count). The engine inserts server objects (sd != 0) at
// the head and client objects at the tail; d3d.ren walks from the tail and stops at the first server object
// (STRICT r_CollectLeafObjects 0x3f3f0). Link data = DObject*.
struct LT1FastNode
{
    LT1Link         m_Objects;              // +0x00 list head
    LT1Node         *m_pNode;               // +0x0c
    LT1FastNode     *m_Sides[2];            // +0x10
};
LT1P_SIZE(LT1FastNode, 0x18)

struct LT1WorldPortal
{
    char            *m_pName;               // +0x00
    lt1_u16         m_Unknown04;
    lt1_u16         m_Flags;                // +0x06
    lt1_u16         m_Index;                // +0x08
    lt1_u16         m_Pad0A;
    LT1Vector       m_Center;               // +0x0c
    LT1Vector       m_Dims;                 // +0x18
};
LT1P_SIZE(LT1WorldPortal, 0x24)

// Fog volume table loaded with the world (STRICT loader). No d3d.ren read of MainWorld +0x08 was found: classic
// fog is the GLOBALPAN_FOGLAYER pan plus the console fog variables.
struct LT1FogTable
{
    lt1_u8          *m_pData;               // +0x00
    lt1_u32         m_DataSize;             // +0x04
    int             m_Dims[3];              // +0x08
    int             m_XSizeTimesYSize;      // +0x14
    LT1Vector       m_Min;                  // +0x18
};
LT1P_SIZE(LT1FogTable, 0x24)

struct LT1LightTable
{
    lt1_u8          *m_pData;               // +0x00 b, g, r, pad per sample
    lt1_u32         m_nData;
    int             m_Dims[3];              // +0x08
    int             m_DimsMinus1[3];        // +0x14
    int             m_XSizeTimesYSize;      // +0x20
    LT1Vector       m_BlockSize;            // +0x24
    LT1Vector       m_InvBlockSize;         // +0x30
    LT1Vector       m_LookupStart;          // +0x3c
};
LT1P_SIZE(LT1LightTable, 0x48)

struct LT1MainWorld
{
    void            *m_pSurfaceEffects;     // +0x00
    LT1WorldBsp     *m_pWorldBsp;           // +0x04
    LT1FogTable     m_FogTable;             // +0x08
    LT1LightTable   m_LightTable;           // +0x2c
    LT1Vector       m_ExtentsMin, m_ExtentsMax, m_ExtentsDiffInv;  // +0x74
    lt1_u32         m_Unknown98;
    void            *m_pLightmapStream;     // +0x9c DStream* kept open for the deferred client lightmap read
                                            // (STRICT BindWorlds 0x115c4 w_ReadLightmapData before CreateContext)
    LT1WorldData    **m_WorldModels;        // +0xa0
    lt1_u32         m_nWorldModels;         // +0xa4
    LT1List         m_WorldModelList;       // +0xa8
    char            *m_pInfoString;         // +0xb8
    lt1_u32         m_Unknown0BC[3];
    void            *m_pRenderContext;      // +0xc8 CreateContext's result (STRICT BindWorlds 0x115ea)
};
LT1P_CHECK(LT1MainWorld, m_LightTable, 0x2c)
LT1P_CHECK(LT1MainWorld, m_pLightmapStream, 0x9c)
LT1P_CHECK(LT1MainWorld, m_WorldModels, 0xa0)
LT1P_CHECK(LT1MainWorld, m_pRenderContext, 0xc8)
LT1P_SIZE(LT1MainWorld, 0xcc)

// ---------------------------------------------------------------- textures
// Exactly 0x24 (STRICT cm_AddSharedTexture Client 0x1f90: bank alloc, `mov ecx,9; rep stosd`). The renderer owns
// +0x08, +0x18 and +0x1c only. Engine-owned fields must never be written by a renderer.
struct LT1SharedTexture
{
    lt1_u32         m_TagFlags;             // +0x00 engine: bit 0 = "used this world" tag; untagged textures are
                                            // freed (NATIVE Client 0x235d `test byte ptr [tex],1` -> free)
    struct LT1TextureData *m_pEngineData;   // +0x04 engine (r_LoadSystemTexture); zeroed around Bind/Unbind
    void            *m_pRenderData;         // +0x08 renderer-owned (0 = not bound); engine zeroes it before Bind
    LT1Link         m_Link;                 // +0x0c engine DLink in CClientMgr +0xa54, data = self
    void            *m_pRendererQueue;      // +0x18 renderer-owned (d3d.ren world / panning-sky queue)
    void            *m_pRendererQueue2;     // +0x1c renderer-owned (d3d.ren gouraud queue, STRICT 0x22a20)
    void            *m_pFile;               // +0x20 engine FileIdentifier* (+0x00 points back here, +0x1c name)
};
LT1P_CHECK(LT1SharedTexture, m_pRenderData, 0x08)
LT1P_CHECK(LT1SharedTexture, m_Link, 0x0c)
LT1P_CHECK(LT1SharedTexture, m_pRendererQueue2, 0x1c)
LT1P_CHECK(LT1SharedTexture, m_pFile, 0x20)
LT1P_SIZE(LT1SharedTexture, 0x24)

struct LT1TextureMip
{
    lt1_u32         m_Width, m_Height, m_Pitch;     // +0x00
    lt1_u8          *m_Data;                // +0x0c 8-bit palette indices
    lt1_u8          *m_AlphaData;           // +0x10 4-bit alpha mask
};
LT1P_SIZE(LT1TextureMip, 0x14)

struct LT1TextureData
{
    lt1_u32         m_Unknown00;
    lt1_u32         m_ResType, m_Version;   // +0x04
    lt1_u16         m_BaseWidth, m_BaseHeight;  // +0x0c
    lt1_u16         m_nMipmaps;             // +0x10
    lt1_u16         m_nSections;            // +0x12 DtxHeader
    lt1_u32         m_DtxFlags;             // +0x14
    lt1_u32         m_UserFlags;            // +0x18
    lt1_u8          m_TextureGroup;         // +0x1c
    lt1_u8          m_nMipmapsToUse;        // +0x1d
    lt1_u8          m_AlphaCutoff;          // +0x1e bit 7 valid; soft.ren alpha test only
    lt1_u8          m_AlphaAverage;         // +0x1f soft.ren only
    lt1_u8          m_Unknown20[4];
    lt1_u32         m_Unknown24;
    LT1Link         m_Link;                 // +0x28
    lt1_u32         m_AllocSize;            // +0x34
    LT1DEPalette    *m_pPalette;            // +0x38
    void            *m_pSections;           // +0x3c DtxSection*
    LT1SharedTexture *m_pSharedTexture;     // +0x40
    lt1_u32         m_Flags;                // +0x44 cache flags (2 = registered)
    LT1TextureMip   m_Mips[1];              // +0x48 flexible: m_nMipmaps entries (allocation 0x48 + n*0x14 + pixels)
};
LT1P_CHECK(LT1TextureData, m_DtxFlags, 0x14)
LT1P_CHECK(LT1TextureData, m_AlphaCutoff, 0x1e)
LT1P_CHECK(LT1TextureData, m_Unknown24, 0x24)
LT1P_CHECK(LT1TextureData, m_pPalette, 0x38)
LT1P_CHECK(LT1TextureData, m_Mips, 0x48)

struct LT1DEPalette
{
    LT1Link         m_Link;                 // +0x00
    void            *m_pRenderData;         // +0x0c
    lt1_u32         m_Keys[2];              // +0x10
    lt1_u8          m_Colors[256][4];       // +0x18 (unused/a, r, g, b)
};
LT1P_CHECK(LT1DEPalette, m_Colors, 0x18)

// ---------------------------------------------------------------- objects
struct LT1DObject
{
    LT1Link         m_WTLink;               // +0x00
    lt1_u8          m_Unknown0C[0x0c];
    void            *m_pBspLinks;           // +0x18
    lt1_u8          m_Unknown1C[0x0c];
    lt1_u32         m_Flags;                // +0x28 FLAG_*
    lt1_u32         m_UserFlags;            // +0x2c
    lt1_u8          m_ColorR, m_ColorG, m_ColorB, m_ColorA;    // +0x30
    struct LT1Attachment *m_Attachments;    // +0x34
    LT1Vector       m_Pos;                  // +0x38
    LT1Rotation     m_Rotation;             // +0x44
    LT1Vector       m_Scale;                // +0x54
    float           m_Radius;               // +0x60
    float           m_Unknown64;
    lt1_u16         m_ObjectID;             // +0x68
    lt1_u16         m_SerializeID;          // +0x6a
    lt1_u16         m_WTFrameCode;          // +0x6c stamped with SceneDesc m_FrameCode when the renderer collects it
    signed char     m_ObjectType;           // +0x6e OT_*
    lt1_u8          m_BPriority;            // +0x6f
    void            *m_pUserData;           // +0x70
    lt1_u8          m_Unknown74[0x11c - 0x74];
    lt1_u16         m_ClientFlags;          // +0x11c
    lt1_u8          m_Unknown11E[6];
    void            *sd;                    // +0x124
};
LT1P_CHECK(LT1DObject, m_Flags, 0x28)
LT1P_CHECK(LT1DObject, m_ColorR, 0x30)
LT1P_CHECK(LT1DObject, m_Pos, 0x38)
LT1P_CHECK(LT1DObject, m_Scale, 0x54)
LT1P_CHECK(LT1DObject, m_ObjectType, 0x6e)
LT1P_SIZE(LT1DObject, 0x128)

struct LT1Attachment
{
    LT1Vector       m_Offset;               // +0x00
    LT1Rotation     m_Rotation;             // +0x0c
    lt1_u16         m_ParentID;             // +0x1c
    lt1_u16         m_ChildID;              // +0x1e
    lt1_u32         m_iSocket;              // +0x20 model node / socket index, 0xffffffff = none
    LT1Attachment   *m_pNext;               // +0x24
};
LT1P_CHECK(LT1Attachment, m_ParentID, 0x1c)
LT1P_SIZE(LT1Attachment, 0x28)

struct LT1Camera
{
    LT1DObject      m_Object;               // +0x000
    int             m_Left, m_Top, m_Right, m_Bottom;  // +0x128 viewport
    float           m_xFov, m_yFov;         // +0x138
    lt1_u32         m_bFullScreen;          // +0x140 dword (cm_Render reads 32 bits)
    LT1Vector       m_LightAdd;             // +0x144 -> SceneDesc m_GlobalLightAdd
};
LT1P_CHECK(LT1Camera, m_bFullScreen, 0x140)
LT1P_SIZE(LT1Camera, 0x150)

enum { LT1_OT_NORMAL, LT1_OT_MODEL, LT1_OT_WORLDMODEL, LT1_OT_SPRITE, LT1_OT_LIGHT, LT1_OT_CAMERA,
       LT1_OT_PARTICLESYSTEM, LT1_OT_POLYGRID, LT1_OT_LINESYSTEM, LT1_OT_CONTAINER };

// One end of the pose bracket (STRICT trk_* in Server.dll/Client.exe).
struct LT1AnimPos
{
    struct LT1ModelAnim *m_pAnim;           // +0x00
    struct LT1AnimKeyFrame *m_pKey;         // +0x04
    lt1_u32         m_Time;                 // +0x08 ms
    lt1_u32         m_iKey;                 // +0x0c key index
};
LT1P_SIZE(LT1AnimPos, 0x10)

// The engine advances it (cm_UpdateAnimations); the renderer only reads it. Pose = lerp(key A, key B, m_Percent)
// with t = (B.m_Time - A.key.time) / (B.key.time - A.key.time), 0 when A == B (STRICT trk_UpdatePositionInterpolant
// Server 0x136c). During a cross-animation transition A and B belong to different animations.
struct LT1AnimTracker
{
    lt1_u8          m_Unknown00[0x0c];
    void            *m_StringKeyCallback;   // +0x0c
    void            *m_pUserData;           // +0x10
    struct LT1Model *m_pModel;              // +0x14
    lt1_u32         m_CurKey;               // +0x18
    lt1_u16         m_Flags;                // +0x1c AT_*
    lt1_u16         m_InterpolationMS;      // +0x1e 0xffff = not interpolating
    LT1AnimPos      m_KeyA;                 // +0x20 bracket key A (d3d.ren reads +0x20 anim, +0x2c index)
    LT1AnimPos      m_KeyB;                 // +0x30 bracket key B; m_KeyB.m_Time (+0x38) is the current time
    float           m_Percent;              // +0x40 pose blend t
};
LT1P_CHECK(LT1AnimTracker, m_KeyA, 0x20)
LT1P_CHECK(LT1AnimTracker, m_KeyB, 0x30)
LT1P_CHECK(LT1AnimTracker, m_Percent, 0x40)
LT1P_SIZE(LT1AnimTracker, 0x44)

struct LT1ModelInstance
{
    LT1DObject      m_Object;               // +0x000
    LT1SharedTexture *m_pSkin;              // +0x128
    LT1AnimTracker  m_AnimTracker;          // +0x12c
    lt1_u8          m_Unknown170[0x0c];
    lt1_u8          *m_HiddenNodes;         // +0x17c 0xff-terminated
};
LT1P_CHECK(LT1ModelInstance, m_AnimTracker, 0x12c)
LT1P_CHECK(LT1ModelInstance, m_HiddenNodes, 0x17c)

struct LT1ModelVertex
{
    float           m_Pos[3];               // +0x00
    float           m_U, m_V;               // +0x0c
    signed char     m_Normal[3];            // +0x14 /127
    lt1_u8          m_NodeIndex;            // +0x17
    lt1_u16         m_Remap[2];             // +0x18 role open
};
LT1P_SIZE(LT1ModelVertex, 0x1c)

struct LT1NodeKeyFrame { LT1Vector m_Translation; LT1Rotation m_Quat; };
LT1P_SIZE(LT1NodeKeyFrame, 0x1c)

// Model data: loader STRICT in Client.exe (model_InternalLoad 0x361e0, model_LoadGeometry 0x36960,
// model_LoadNode 0x36c30, model_LoadAnimation 0x36d60). Engine-owned; the renderer reads it.
struct LT1ModelFace
{
    lt1_u16         m_Vertices[3];          // +0x00
    lt1_u8          m_Unknown06[3];         // +0x06 role OPEN
    lt1_u8          m_NodeSelector;         // +0x09 node-visibility selector
};
LT1P_SIZE(LT1ModelFace, 0x0a)

struct LT1ModelNode
{
    char            *m_pName;               // +0x00
    LT1Vector       m_BoxMin, m_BoxMax;     // +0x04
    lt1_u32         m_Unknown1C;
    lt1_u16         *m_pDeformVertices;     // +0x20
    lt1_u32         m_nDeformVertices;      // +0x24
    lt1_u32         m_Unknown28;
    lt1_u16         m_NodeIndex;            // +0x2c
    lt1_u16         m_Flags;                // +0x2e 1 null, 2 has tris, 4 deformation (dequantize via AnimNode)
    LT1ModelNode    *m_Children;            // +0x30 array, stride 0x38
    lt1_u32         m_nChildren;            // +0x34
};
LT1P_CHECK(LT1ModelNode, m_Flags, 0x2e)
LT1P_SIZE(LT1ModelNode, 0x38)

struct LT1AnimKeyFrame
{
    lt1_u32         m_Time;                 // +0x00 ms
    LT1Vector       m_BoxMin, m_BoxMax;     // +0x04
    char            *m_pString;             // +0x1c string key command
    lt1_u8          m_KeyType;              // +0x20
    lt1_u8          m_Pad21[3];
    void            *m_pCallback;           // +0x24
    void            *m_pUser;               // +0x28
};
LT1P_CHECK(LT1AnimKeyFrame, m_pString, 0x1c)
LT1P_SIZE(LT1AnimKeyFrame, 0x2c)

struct LT1AnimNode
{
    LT1ModelNode    *m_pNode;               // +0x00
    struct LT1ModelAnim *m_pAnim;           // +0x04
    void            *m_pVertexData;         // +0x08 compressed deformation data
    lt1_u32         m_Unknown0C;
    LT1Vector       m_Scale;                // +0x10 per (anim, node) dequantization
    LT1Vector       m_Translation;          // +0x1c
    LT1NodeKeyFrame *m_KeyFrames;           // +0x28 one per AnimKeyFrame
    LT1AnimNode     *m_pParent;             // +0x2c
    LT1AnimNode     *m_Children;            // +0x30 array, stride 0x34
};
LT1P_CHECK(LT1AnimNode, m_KeyFrames, 0x28)
LT1P_SIZE(LT1AnimNode, 0x34)

struct LT1ModelAnim
{
    char            *m_pName;               // +0x00
    lt1_u32         m_Length;               // +0x04 ms
    LT1Vector       m_Dims;                 // +0x08
    void            *m_pFlaggedNodeData;    // +0x14 freed after load
    LT1Vector       m_BoxMin, m_BoxMax;     // +0x18
    float           m_Radius;               // +0x30
    LT1AnimKeyFrame *m_KeyFrames;           // +0x34
    lt1_u32         m_nKeyFrames;           // +0x38
    void            *m_pNodeData;           // +0x3c freed after load
    LT1AnimNode     m_RootNode;             // +0x40 (pose builder starts at +0x40 +0x28 = root keys)
};
LT1P_CHECK(LT1ModelAnim, m_KeyFrames, 0x34)
LT1P_CHECK(LT1ModelAnim, m_RootNode, 0x40)
LT1P_SIZE(LT1ModelAnim, 0x74)

struct LT1Model
{
    LT1Link         m_Link;                 // +0x00 data = self
    char            *m_pFilename;           // +0x0c
    lt1_u32         m_GeometryBytes;        // +0x10
    lt1_u32         m_AnimationBytes;       // +0x14
    LT1Vector       m_BoxMin, m_BoxMax;     // +0x18
    float           m_Radius;               // +0x30
    lt1_u8          m_Nexus[0x0c];          // +0x34
    void            *m_pFileID;             // +0x40
    lt1_u32         m_ModelFlags;           // +0x44 MODELFLAG_*
    LT1ModelNode    m_RootNode;             // +0x48 embedded
    LT1ModelNode    **m_FlatNodes;          // +0x80 all nodes
    lt1_u32         m_nNodes;               // +0x84
    void            *m_FlaggedNodes;        // +0x88
    lt1_u32         m_nFlaggedNodes;        // +0x8c
    lt1_u32         m_Unknown90;
    lt1_u32         m_nVertices;            // +0x94 total (all LOD levels)
    LT1ModelVertex  *m_Vertices;            // +0x98
    lt1_u32         m_nBaseVertices;        // +0x9c
    lt1_u32         m_nFaces;               // +0xa0
    LT1ModelFace    *m_Faces;               // +0xa4
    float           *m_FaceUVs;             // +0xa8 3 (u, v) texel pairs per face = 0x18 bytes per face
    void            *m_Work;                // +0xac
    lt1_u32         m_nLODs;                // +0xb0
    float           m_LODStartDist;         // +0xb4
    float           m_LODIncrement;         // +0xb8
    float           m_LODMaxDist;           // +0xbc beyond it the model is not drawn
    float           m_MipmapDistAdd;        // +0xc0
    lt1_u16         *m_LODVertexStarts;     // +0xc4
    LT1Matrix       *m_NodeMatrices;        // +0xc8 written by the renderer's pose builder
    lt1_u32         m_nNodeMatrices;        // +0xcc
    char            *m_pCommandString;      // +0xd0
    void            *m_StringList;          // +0xd4
    float           m_DrawIndexedDist;      // +0xd8 d >= it: the far (indexed, per-vertex UV) path
    lt1_u32         *m_StripLengths;        // +0xdc
    lt1_u32         m_nStrips;              // +0xe0
    lt1_u16         *m_StripIndices;        // +0xe4
    lt1_u32         m_nStripIndices;        // +0xe8
    void            *m_Triangles;           // +0xec
    lt1_u32         m_nTriangles;           // +0xf0
    LT1ModelAnim    *m_Anims;               // +0xf4
    lt1_u32         m_nAnims;               // +0xf8
};
LT1P_CHECK(LT1Model, m_RootNode, 0x48)
LT1P_CHECK(LT1Model, m_nBaseVertices, 0x9c)
LT1P_CHECK(LT1Model, m_FaceUVs, 0xa8)
LT1P_CHECK(LT1Model, m_LODVertexStarts, 0xc4)
LT1P_CHECK(LT1Model, m_DrawIndexedDist, 0xd8)
LT1P_CHECK(LT1Model, m_Anims, 0xf4)
LT1P_SIZE(LT1Model, 0xfc)

struct LT1WorldModelInstance
{
    LT1DObject      m_Object;               // +0x000
    LT1WorldData    *m_pWorldData;          // +0x128
    LT1Matrix       m_Transform;            // +0x12c local -> world
    LT1Matrix       m_BackTransform;        // +0x16c world -> local
};
LT1P_CHECK(LT1WorldModelInstance, m_Transform, 0x12c)
LT1P_CHECK(LT1WorldModelInstance, m_BackTransform, 0x16c)
LT1P_SIZE(LT1WorldModelInstance, 0x1ac)

struct LT1DynamicLight { LT1DObject m_Object; float m_LightRadius; };      // colour = DObject +0x30, pos +0x38
LT1P_CHECK(LT1DynamicLight, m_LightRadius, 0x128)

struct LT1PSParticle
{
    LT1Vector       m_Velocity;             // +0x00
    LT1Vector       m_Color;                // +0x0c 0..255
    LT1PSParticle   *m_pNext;               // +0x18
    LT1Vector       m_Pos;                  // +0x1c
    float           m_Lifetime;             // +0x28
    LT1PSParticle   *m_pPrev;               // +0x2c
};
LT1P_SIZE(LT1PSParticle, 0x30)

struct LT1ParticleSystem
{
    LT1DObject      m_Object;               // +0x000
    LT1PSParticle   m_ParticleHead;         // +0x128 (first = m_ParticleHead.m_pNext at +0x140)
    void            *m_pParticleBank;       // +0x158
    LT1SharedTexture *m_pTexture;           // +0x15c
    lt1_u8          m_SoftwareR, m_SoftwareG, m_SoftwareB, m_Pad163;   // +0x160
    lt1_u8          m_Unknown164[0x17c - 0x164];
    LT1Vector       m_SystemCenter;         // +0x17c world space
    float           m_SystemRadius;         // +0x188
    lt1_u8          m_Unknown18C[0x19c - 0x18c];
    lt1_u32         m_nParticles;           // +0x19c
    lt1_u32         m_nChangedParticles;    // +0x1a0
    LT1Vector       m_MinPos, m_MaxPos;     // +0x1a4
    float           m_GravityAccel;         // +0x1bc
    float           m_ParticleRadius;       // +0x1c0
    lt1_u32         m_PSFlags;              // +0x1c4
};
LT1P_CHECK(LT1ParticleSystem, m_pTexture, 0x15c)
LT1P_CHECK(LT1ParticleSystem, m_SystemCenter, 0x17c)
LT1P_CHECK(LT1ParticleSystem, m_nParticles, 0x19c)
LT1P_CHECK(LT1ParticleSystem, m_PSFlags, 0x1c4)

struct LT1PolyGrid
{
    LT1DObject      m_Object;               // +0x000
    signed char     *m_Data;                // +0x128 heights
    lt1_u16         *m_pIndices;            // +0x12c
    void            *m_pSprite;             // +0x130
    lt1_u8          m_SpriteTracker[0x14];  // +0x134 (current frame record at +0x13c)
    LT1SharedTexture *m_pEnvMap;            // +0x148
    float           m_xPan, m_yPan;         // +0x14c
    float           m_xScale, m_yScale;     // +0x154
    lt1_u32         m_nTriangles;           // +0x15c
    lt1_u32         m_nIndices;             // +0x160
    LT1Link         m_LeafLinks;            // +0x164
    lt1_u32         m_Width, m_Height;      // +0x170 integers
    LT1Rotation     m_ColorTable[256];      // +0x178 RELEASED PGColor {x, y, z, a} = r, g, b, a in 0..255
};
LT1P_CHECK(LT1PolyGrid, m_pEnvMap, 0x148)
LT1P_CHECK(LT1PolyGrid, m_LeafLinks, 0x164)
LT1P_CHECK(LT1PolyGrid, m_Width, 0x170)
LT1P_CHECK(LT1PolyGrid, m_ColorTable, 0x178)
LT1P_SIZE(LT1PolyGrid, 0x178 + 256 * 0x10)

struct LT1LineSegment
{
    LT1Vector       m_Start;                // +0x00
    float           m_StartColor[4];        // +0x0c RGBA
    LT1Vector       m_End;                  // +0x1c
    float           m_EndColor[4];          // +0x28 RGBA
    void            *m_pSystem;             // +0x38 owning LineSystem
    LT1LineSegment  *m_pPrev;               // +0x3c
    LT1LineSegment  *m_pNext;               // +0x40
};
LT1P_SIZE(LT1LineSegment, 0x44)

// STRICT linesystem_* / cm_UpdateLineSystems (Client.exe).
struct LT1LineSystem
{
    LT1DObject      m_Object;               // +0x000
    void            *m_pLineBank;           // +0x128
    lt1_u32         m_bChanged;             // +0x12c
    LT1LineSegment  m_LineHead;             // +0x130 (first = +0x170; empty when it points back at +0x130)
    LT1Vector       m_MinPos, m_MaxPos;     // +0x174
    LT1Vector       m_SystemCenter;         // +0x18c
    float           m_SystemRadius;         // +0x198
};
LT1P_CHECK(LT1LineSystem, m_LineHead, 0x130)
LT1P_CHECK(LT1LineSystem, m_SystemRadius, 0x198)
LT1P_SIZE(LT1LineSystem, 0x19c)

// Sprite data: sprite_lt1.h (WRITTEN; consumers STRICT LoadSprite 0x58970, SpriteInit 0x41a45).
struct LT1SpriteAnim
{
    char            m_Name[32];             // +0x00
    LT1SharedTexture **m_Frames;            // +0x20
    lt1_u32         m_nFrames;              // +0x24
    lt1_u32         m_AnimLength;           // +0x28 ms
    lt1_u32         m_MSFrameRate;          // +0x2c
    lt1_u8          m_bKeyed, m_Pad31[3];   // +0x30
    lt1_u32         m_ColourKey;            // +0x34
    lt1_u8          m_bTranslucent, m_Pad39[3];    // +0x38
};
LT1P_SIZE(LT1SpriteAnim, 0x3c)

struct LT1Sprite { LT1Link m_Link; LT1SpriteAnim *m_Anims; lt1_u32 m_nAnims; };
LT1P_SIZE(LT1Sprite, 0x14)

struct LT1SpriteTracker
{
    LT1Sprite       *m_pSprite;             // +0x00
    LT1SpriteAnim   *m_pCurAnim;            // +0x04
    LT1SharedTexture **m_pCurFrame;         // +0x08 current frame entry (entry +0 = SharedTexture*)
    lt1_u32         m_MSCurTime;            // +0x0c
    lt1_u32         m_Flags;                // +0x10
};
LT1P_SIZE(LT1SpriteTracker, 0x14)

struct LT1SpriteInstance
{
    LT1DObject      m_Object;               // +0x000
    LT1SpriteTracker m_Tracker;             // +0x128
    lt1_u32         m_ClipperPoly;          // +0x13c hPoly: 0xffffffff = no clip (STRICT SpriteInit); high word =
                                            // world model (0xffff = main world), low word = node index
    void            *m_SCImplVtbl;          // +0x140 SpriteControl
    void            *m_SCImplSelf;          // +0x144 = this
};
LT1P_CHECK(LT1SpriteInstance, m_ClipperPoly, 0x13c)
LT1P_SIZE(LT1SpriteInstance, 0x148)

#endif
