// lithray.ren internals shared between shim.cpp (ABI slots, 2D layer, window) and scene.cpp (snapshots).
#pragma once
#include "lt1_abi.h"
#include "bridge.h"

extern LT1RenderStruct *g_rs;

float CVar(const char *name, float fallback);

// textures (shim.cpp)
uint32_t TexId(LT1SharedTexture *tex);      // binds lazily; 0 when null or unavailable

// scene.cpp
void *Scene_CreateContext(LT1RenderContextInit *init);
void Scene_DeleteContext(void *context);
void Scene_Render(LT1SceneDesc *scene);
bool Scene_HaveWorld();
void Scene_Reset();                          // renderer instance going away: forget models and the world
