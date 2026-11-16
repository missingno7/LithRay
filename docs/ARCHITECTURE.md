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

Frame = opaque pass -> lighting resolve -> translucent pass -> post.

1. **Opaque pass** (`scene.frag`, compiled twice: raster and `-DRT`): sky, world, solid objects and the view model
   into colour, aux (motion vectors, AO, packed normal), light and depth. A pixel is either *final* (`light.a = 0`:
   sky, emissive, view model, raster mode) or *composed* (`light.a = 1`: colour holds the albedo and `light` the
   per-pixel light estimate).
2. **Lighting resolve** (`compose.frag`): filters the light estimate across neighbours on the same surface (depth +
   normal weights), blends it with the reprojected history, multiplies it back onto the albedo, adds the texture's
   fullbright layer unfiltered, applies fog. Writes the lit scene and the new light history.
3. **Translucent pass**: every blended group over the lit scene, depth tested, in d3d.ren's fixed group order.
4. **Post** (`post.frag`): ambient occlusion (lightmap mode only), camera light add, the 2D layer, swapchain.

Lighting modes:

* **Raster baseline (`lr_RT 0`)**: texture x lightmap x light scale (`Saturate` doubles), fullbright texels,
  LithTech's 16-step half-Lambert model ramp from ambient + light-grid directional, signed dynamic lights without
  occlusion, distance fog, one-sided world polygons.
* **Lightmaps with ray-traced shadows (`lr_RT 1`, `lr_RTGI 0`)**: world surfaces keep their lightmap and lose a lamp's
  estimated contribution where a ray to it is blocked by something that was not in the bake (models, doors, masked
  walls); models are lit by the lamps with shadow rays; ambient occlusion from short rays.
* **Full ray-traced lighting (`lr_RT 1`, `lr_RTGI 1`, the default)**: no lightmap and no light grid on opaque world
  and model surfaces. Per pixel: every lamp in reach with one shadow ray towards a jittered point of the lamp
  (soft edges once accumulated), plus one cosine-distributed bounce ray whose hit is lit by one randomly chosen
  lamp (with its own shadow ray), the level ambient, emissive texels or the sky. The estimate is denoised by the
  resolve pass.

Where the light comes from (`host/lamps.cpp`):

* **The level's static lights** exist only in the world file (the engine never creates them: `CF_NORUNTIME`). The host
  reads `Light` / `DirLight` / `ObjectLight` objects from the game's rez files and keeps them in a uniform grid.
* **Emitters**: polygons whose texture has fullbright texels (palette entries 246..255 of a `DTX_FULLBRITE` texture
  -- neon lines, lamp panels, signs), plus polygons the level marks bright (surface flag 0x20, a saturated lightmap, or
  full-white baked vertex colours without a lightmap). Each becomes a cosine-lobed lamp in front of the polygon,
  coloured by the texture's average (fullbright) colour and sized by the polygon; it goes through the same code as
  the file lamps. The emitter list is built once the surface textures are known.
* **Dynamic lights** (`OT_LIGHT` objects, signed colours) with one shadow ray each.

Calibration notes (measured against the lightmap mode at the same camera): the original bake has no cosine term
(`colour * BrightScale * (1 - d/r) * cone`, one-sided) and clamps at white. Pure Lambert made every wall and ceiling
visibly darker than the designers' result, so the direct term uses `0.5 + 0.5 n.l`, and the resolve applies a soft
shoulder above mid-grey instead of the bake's clamp. `lr_GIDirect`, `lr_GIIndirect`, `lr_GIEmissive` scale the three
contributions.

Reflections (one mirror ray, hit shaded by the lamps in full mode or by its lightmap otherwise) follow the engine's
own flags: a model with `FLAG_ENVIRONMENTMAP` reflects by `1 - object alpha` (d3d.ren's chrome pass blends the skin
over the environment map by that alpha), a polygrid reflects when it has an environment map. A keyword table on
texture names adds a Fresnel-weighted gloss to some world surfaces; that part is not game data.

Sky (reconstruction `port_notes/sky.md`): drawn first from the sky camera, which sits at the player's fractional
position through the world box mapped into the sky view box; objects in list order, each one covering the previous
(depth is cleared before every sky object, which reproduces the original's painter's pass while letting each
object sort itself); front faces only, even for translucent sky world models; fogged with the sky fog range from the
sky camera. In blended passes a texture's alpha is opacity, including the alpha bit of fullbright texels (how the
moon's surround is transparent).

Ray queries alpha-test: masked world models (fences, grates) are non-opaque geometry in the TLAS (mask 8) and every
candidate hit samples the texture's alpha. TLAS masks: 1 world, 2 dynamic models, 4 solid world models, 8 masked.

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
