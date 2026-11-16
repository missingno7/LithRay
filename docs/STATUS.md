# Status

Last updated 2026-11-16 (first session, with the user playing the game during the second half). "Verified" means seen working in the real game on this machine
(RTX 4090, Blood II 2.1 in `D:\Prog\blood2_test`); everything else is labelled.

## Verified in the running game

* The original `CLIENT.EXE` loads `lithray.ren`, which starts `lithray_host.exe`; frames cross the 32→64-bit bridge
  and are presented in the game's window. Clean start and clean exit, repeatedly.
* Main menu, HUD, console text (2D layer).
* `01_SubwayA`: world geometry, textures, lightmaps, world models (doors, train), translucent glass, animated
  characters with skins, attachments (weapons in hands), the first-person weapon, dynamic lights, decal sprites
  clipped to their polygon, one-sided world polygons.
* Ray tracing: static BLAS per BSP, per-frame dynamic BLAS + TLAS; **lamp shadows** from the level's own lights
  (371 lamps read from `Worlds\01_SubwayA.dat`), ambient occlusion with temporal accumulation. About 2.3 ms GPU at
  1280×720; uncapped at 1920×1000 the game runs at roughly 370-730 fps with ray tracing on.
* `lr_RT 0` raster baseline of the same scene.
* **Full ray-traced lighting** (`lr_RTGI 1`): lamps with shadow rays + one bounce + emitters, resolved with spatial and
  temporal filtering; brightness matched by eye against the lightmap mode in the train carriage. 240 fps at 1280x720
  (about 1.5 ms GPU); at 3840x2160 about 5.5-6 ms GPU, 145-165 fps.
* Borderless fullscreen at 3840x2160, played interactively by the user in `01_SubwayA` and the following city level.
* **Alt+Tab**: the engine unloads the renderer, the host survives and the next `Init` reconnects (seen three times
  in one session, no errors).
* Sky and moon after the sky rewrite (the user: "about right", not verified pixel-exact against d3d.ren).
* Masked walls (fence): two-sided, alpha-tested shadows (the user: "looks right").
* Per-pixel depth-sorted transparency and scene-lit decals: blood behind fence bars is ordered and lit correctly
  (confirmed by the user in the city level). Water: "looks good". Glass visible after the per-texel split.

## Bugs found by running the game (and their real causes)

* **Characters exploding into a screen-crossing "plane" for a few frames** (first cutscene). Cause: animation key
  quaternions are not unit length and d3d.ren's quat-to-matrix (0x3e1d0) divides by |q|^2; the shim assumed unit
  quaternions, which scaled every bone down the hierarchy for some keys. Fixed in `shim/scene.cpp` `QuatToM`. Found
  with `lr_DebugView 6` (one colour per instance) on a burst of early frames plus the instance table the host logs
  with such a screenshot, then the shim's `BAD POSE` diagnostic (still in place: it must stay silent).
  Two earlier guesses (unclipped decals, two-sided world polygons) were real deviations and are fixed too, but they
  were not this bug: identify the offending instance before changing anything.
* Untextured models: model UVs are normalised, world UVs are texels.
* Missing first-person weapon / sky objects in the world: object flag constants written from memory were wrong.
* 100 fps cap never lifted: a missing `server.dll` was treated as a failed game-speed patch. Single player runs the
  server inside `CLIENT.EXE`; a module that is not loaded is fine, as in d_ren.

* Sky flat fog colour: the sky was fogged by its distance from the player camera instead of the sky camera.
* Sky looked wrong / the moon was an opaque box: the sky is a painter's pass with front faces only, and in blended
  passes texture alpha is opacity (also for the fullbright alpha bit). Both from `port_notes/sky.md` and E.1.13.
* Masked walls one-sided and unlit: translucent world models are two-sided, pre-lit by vertex colours, no lightmap.
* View pistol with see-through parts: d3d.ren never alpha-tests; a solid model ignores its skin's alpha mask.
* Menu image stuck over a cutscene: an empty 2D layer is not uploaded, and the host kept compositing the last image.
* Full ray-traced mode washed out, shadows blurred: (1) emitters double-counted light wherever the level already has a
  lamp under a glowing panel (now skipped there); (2) direct and bounce light shared one filtered buffer (now direct
  light is exact and unfiltered, only the bounce is denoised). The lamp values themselves are right: fitted against
  the level's lightmaps the unshadowed lamp sum has a median ratio of 0.93 in `01_SubwayA`, which also confirms the
  bake formula (no cosine term). `host/lamps.cpp` `calibrateLamps` logs this per level.
* Full ray-traced mode dimmer than the original: the bake has no cosine term; and "bright" things are not tagged
  polygons in this level (no surface has flag 0x20) but fullbright texels and ordinary lightmapped world models.

* Blended surfaces in the wrong order (a far fence over a near one, blood behind bars drawn over them): the original
  blends in a fixed group order with no sorting. Two attempts that classified textures as "fence" or "glass" by alpha
  statistics were workarounds and were removed. The fix is structural: solid texels of a masked wall go to the opaque
  pass (per texel), and all blended fragments are sorted per pixel by depth before compositing.

## Implemented but not yet verified in game

* Mode switch from the options menu.
* Polygrids (water) and their reflections, particle systems, line systems, glow/bias/no-Z sprites, fog.
* The modern look (HDR float pipeline, 10-bit output, bloom, filmic tone mapping), soft area-light shadows and
  volumetric light: seen in scripted captures of the train level only; not yet judged by the user, HDR10 output
  (`lr_HDR 1`) never run, 4K cost unmeasured.
* Generated normal maps and keyword specular (seen working in the train level's light buffer; look and strength not
  yet judged by the user). Polygrid normals from real grid spacing (no water seen yet).
* Chrome models reflecting by `1 - alpha` and the removal of the model alpha test (the view pistol showed see-through
  parts before it): built and deployed, not yet confirmed by the user.
* Emitters lighting their surroundings in levels other than the train.
* Reflections in general (the keyword table needs a level with matching textures to judge).
* Texture UV animation updates (surface effects), vertex-animated models beyond what the first level uses.
* F8 screenshots (BMP path), host loss handling, arena overflow flushes.
* Devices without ray tracing (raster fallback path compiles but has not run).

## Known deviations from the original renderer

* No PVS / portal culling: everything is drawn and the depth buffer sorts it out. The "was drawn" feedback
  (`FLAG_INTERNAL1`) for particle systems and polygrids uses a coarse in-front-of-camera test, so more systems stay
  active than under d3d.ren. `DObject +0x6c` frame-code stamps are not written.
* Textures are converted at full 8-bit palette precision (d3d.ren quantises to 1555 / 4444).
* Model lighting is per pixel (the 16-step ramp is reproduced, but evaluated per fragment); dynamic lights are
  added per pixel instead of into the model's ambient term.
* Model LOD, model tint pass, environment-map (chrome) pass, d3d.ren's flattened model shadows, model warble,
  cloud-shadow pan (`SURF_PANNINGSKY`), `LightAddPoly` ordering, table fog: not implemented (fog is by eye distance).
* Smacker videos are skipped (`GetHook` returns null), as in d_ren.
* World polygons use their base fan only (T-junction fix vertices are ignored).
* 3D is rendered at window resolution with Hor+ FOV by default (`lr_Widescreen 0` for the game's FOV).

## Open problems

2. `+runworld` with the install's custom rez arguments stays in the menu (TESTING.md).
3. "Disconnected from the server" message at every `+runworld` start: cause unknown, harmless so far.
4. Plain translucent world models (glass) are not in the TLAS; translucent surfaces are lit the classic way and are
   not part of the denoised lighting.
5. `GlobalDirLight` (sun) lamps are parsed but ignored; spot-lamp direction from the Euler rotation is unverified
   (no visible spot shadow has been checked against the bake).
6. The full ray-traced mode accumulates over about ten frames: fast-moving shadows and lights lag, and there is no
   history-length tracking or variance guidance. Reflections are folded into the light buffer and get blurred with it.
   In blended passes a plain (non-fullbright, non-masked) texture is treated as opaque alpha; the 1555 format rule
   would make only palette entries 246..255 opaque there -- unresolved, see E.1.3 / E.1.5 of the reconstruction.
7. AO is applied in the resolve pass to the whole pixel, including translucent surfaces drawn over it.
8. Object identity is the engine object address. Slots are reused, so history can mis-associate for one frame.
9. The dynamic geometry budget is fixed (500k vertices / 1.5M indices per frame); overflow drops objects silently.
10. No parity capture against d3d.ren / d_ren has been made yet; colours and brightness are judged by eye only.

## Next steps (suggested order)

0. Tune and verify the full ray-traced mode on more levels (outdoors, water, dark areas); half-resolution lighting
   for 4K.
1. Parity harness: same camera, d3d.ren (dgVoodoo2) vs d_ren vs LithRay raster, diffed (TESTING.md step 4).
2. Exercise the unverified list on an outdoor level with water (sky, polygrids, particles, fog), then Alt+Tab.
3. Glass in the TLAS (tinted transmission); ray-traced lighting for translucent surfaces.
4. A proper denoiser (history length, variance, separate specular buffer).
5. Classic-mode completeness: chrome pass, model tint, cloud shadows, LOD.
6. Verify fullscreen and 4K performance.
