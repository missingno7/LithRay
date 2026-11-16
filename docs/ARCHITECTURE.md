# LithRay architecture

LithRay renders Blood II: The Chosen (LithTech 1.0, 32-bit, unmodified `CLIENT.EXE`) with a native x64 Vulkan
renderer that uses hardware ray tracing. The game keeps doing gameplay, simulation, sound, input and resource
loading. LithRay replaces only the renderer plugin.

```
CLIENT.EXE (x86, original)                         lithray_host.exe (x64)
 └─ lithray.ren  (shim, x86)        shared memory   ├─ decode: copy commands into the scene store
     • the LT1 renderer ABI         ─────────────►  ├─ scene: world, textures, models, lamps, history
     • snapshots engine memory        commands      ├─ Vulkan 1.3: raster + ray queries (TLAS/BLAS)
     • 2D layer (CPU, 16-bit)       ◄─────────────  └─ presents into a child window of the game window
     • compatibility patches          ack / status
```

## Why two processes

The engine loads its renderer as a 32-bit DLL and hands it raw pointers into engine memory. A 64-bit renderer
cannot live in that process, and the existing 32-bit Vulkan renderer (`D:\Prog\LithTech-1-Renderer`, "d_ren")
cannot simply be recompiled: its code is written against 32-bit engine structures. So the boundary is drawn where
the reconstruction (`blood2_recon/docs/renderer_modern_interface.md`) proposed it:

* **Shim** (`shim/`, C++, x86, ~1,300 lines): everything that touches engine pointers or engine rules.
* **Host** (`host/`, C++20, x64): everything that touches the GPU. It never sees an engine pointer.
* **Protocol** (`common/protocol.h`): plain fixed-width records. This one header is the whole contract.

## Bridge

* One named file mapping `Local\LithRay.<gamePid>.map` = 4 KB header + a 64 MB linear command arena, and two events
  (`submit`, `done`).
* The shim appends commands. A *flush* sets `header.used`, signals `submit` and waits for `done`.
* The host decodes the arena — copying everything it will need — signals `done`, and **only then renders**. So the
  game simulates frame N+1 while the host draws frame N (a two-stage pipeline with one frame in flight), and the
  host never reads shim memory outside the flush window.
* A flush happens at `SwapBuffers`, after `CreateContext` (world upload), after `Init`/`Term`, and whenever the
  arena is full.
* Failure handling: every wait is also a wait on the other process. If the host dies, the shim logs it and all
  slots become no-ops (the game keeps running without a picture rather than hanging). If the game dies, the host
  exits.

### Ownership and lifetime

| thing | owner | notes |
|---|---|---|
| engine memory (world, objects, textures) | engine | read only inside a slot call; never after it returns |
| 2D surfaces, the 16-bit "screen", texture/model id tables | shim | die with the renderer instance |
| every GPU object, scene store, temporal history | host | survives renderer re-initialisation |
| host process | started by the shim's first `Init` | exits with the game process |

The engine unloads the renderer DLL on every focus loss and mode switch (`r_TermRender`, synchronously from the
window procedure). The host therefore outlives the shim: `Term` only resets the host's scene state; the next `Init`
finds the mapping, sees a live `hostPid` and reconnects without recreating the Vulkan device.

### Presentation

The host creates a disabled `WS_CHILD` window covering the game window's client area and builds its swapchain on
it. The child takes no focus and no input (`WS_DISABLED`, `HTTRANSPARENT`), so the engine's own window keeps
handling keyboard, mouse and activation exactly as before. A dedicated host thread pumps the child's messages so
cross-process window messages never wait on rendering. The shim sizes the game window (borderless on the primary
monitor at 0,0, or a mode-sized client area with `windowed 1`); the host follows the parent's client size.

The engine's 2D drawing (menus, HUD, console, fonts) goes into a CPU-side RGB565 "screen" plus a coverage mask in
the shim, exactly like d_ren. It is sent when it changed and composited over the 3D frame at mode resolution.

## Scene representation (designed for ray tracing)

The shim does **not** send what is visible. It sends what exists:

* **World** (once per `CreateContext`): every BSP of the level — the main world and each world model in its original
  space — as polygons with texel-space UVs, lightmap UVs and the RGB565 lightmap source (copied immediately: the
  engine frees it before `DeleteContext`), plus the model light grid, the world file name and the `-rez` sources.
* **Per frame** (`RenderScene`): camera, sky definition, console settings, the texture of every world surface
  (animated textures swap pointers silently), changed texture coordinates (surface effects), and **every client
  object in the world tree** — the FastNode walk d3d.ren uses for its "draw all" path, plus polygrids from the
  leaves, attachments through the engine callback, and the sky objects. Not just the PVS-visible set.
* **Models**: geometry once per model (`ModelDef`). Per frame the shim evaluates the pose (it owns the quirks:
  transposed key quaternions, deformation-node affines, vertex animation written into engine memory, the game's
  model hook) and sends node matrices; the host skins on the CPU into world space.
* **Textures**: raw 8-bit indices + palette + 4-bit alpha per mip, converted in the host.

In the host all geometry lives in **one vertex buffer and one index buffer** shared by rasterisation and ray
tracing, with small tables on top (`shaders/common.glsl`):

* `Instance` — per draw / per TLAS instance: transform, previous transform, material and lighting inputs.
* `triTable` — per triangle: the world surface (static geometry) or the owning instance (dynamic BLAS).
* `Surface`, `TexInfo`, bindless `textures[]`, `Light` (dynamic), `Lamp` + lamp grid (static level lights).

A ray hit resolves `TLAS custom index → Instance → indices → vertices → surface/texture`, so reflections shade
hits with the same data the rasteriser uses.

Acceleration structures: one static BLAS per BSP (world models are TLAS instances with their transform), one
dynamic BLAS per frame containing every opaque model, one TLAS per frame. TLAS masks: `1` world, `2` dynamic
models, `4` world models.

Temporal data: per-object history keyed by the engine object address (previous transform, previous frame's
vertices in the alternate half of the dynamic buffer) gives motion vectors; the ambient-occlusion term is
accumulated along them with depth rejection.

## Rendering

One forward pass (`scene.frag`, compiled twice: raster and `-DRT`) into colour + aux (motion, AO) + depth, then a
resolve pass (`post.frag`) that filters/accumulates AO, applies the camera light add and the 2D layer, and writes
the swapchain.

* **Raster baseline (`lr_RT 0`)**: texture × lightmap × light scale (`Saturate` doubles), fullbright texels,
  LithTech's 16-step half-Lambert model ramp from ambient + light-grid directional, signed dynamic lights without
  occlusion, distance fog, d3d.ren's fixed draw-group order with no depth sorting, one-sided world polygons.
* **Ray traced (`lr_RT 1`)**:
  * *Lamp shadows.* The level's static lights exist only in the world file (the engine never creates them). The
    host reads them from the game's rez files. World surfaces keep their baked lightmap and lose a lamp's estimated
    contribution where a ray to it is blocked by something that was **not** in the bake (models, doors) — and never
    more light than the lightmap shows arrived. Models are lit by the lamps directly with shadow rays against
    everything, replacing the matching share of the light-grid term.
  * *Dynamic light shadows* (ray per light), *ambient occlusion* (2 rays, filtered + accumulated),
    *reflections* (one mirror ray, hit shaded classically; reflectivity from a texture-name keyword table,
    environment-mapped models and water).

All shading stays in the game's display-referred 8-bit space so the baseline matches the original look.

## Compatibility work in the shim

Ported from d_ren where it had already been proven: cursor guard (`SetCursorPos` IAT patch), 1 ms input clock
(`GetTickCount` IAT patch), the 0.01 s frame-time floor patch for Blood II 2.1 (falls back to a 100 fps cap when
the instructions do not match), background mode for unattended tests (`lr_Background 1` swallows
`WM_ACTIVATEAPP(FALSE)`), `FLAG_INTERNAL1` feedback for particle systems and polygrids, non-null `CreateContext`.

## Sources of truth

* `D:\Prog\blood2_recon` — ABI and semantics. Entry point `docs/renderer_closure.md` (A–E), canonical header
  `recon/renderer/include/lt1_port_abi.h` (copied to `shim/lt1_abi.h`). Its docs lag its code; trust
  `recon/state/*.tsv` for what is proven.
* `D:\Prog\LithTech-1-Renderer` — a working in-process renderer: practical behaviour and a visual comparison.
* The released SDK headers (`blood2_recon/assets/blood2src/AppHeaders`) for flag values. Do not guess constants:
  the first LithRay build had wrong object flags because they were written from memory.
