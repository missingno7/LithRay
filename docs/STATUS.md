# Status

Last updated 2026-11-16 (first session). "Verified" means seen working in the real game on this machine
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
  1280×720.
* `lr_RT 0` raster baseline of the same scene.

## Implemented but not yet verified in game

* Alt+Tab / mode switch (the host is designed to survive the DLL unload and be reconnected; never exercised).
* Borderless fullscreen (`-Interactive` without `-Windowed`); resolutions other than 1280×720.
* Sky objects and the sky camera, particle systems, polygrids (water) and their reflections, line systems,
  glow/bias/no-Z sprites, fog: the first level shows none of them clearly.
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

1. **Game-speed patch on `server.dll` never applies** (`game speed fix: client 1 server 0` in every run), so the
   frame rate stays capped at 100. The addresses are d_ren's (RVA 0x42ba8, operands 0x26df1 / 0x26e07); check the
   module name the engine loads the server under and whether d_ren's log shows it applying.
2. `+runworld` with the install's custom rez arguments stays in the menu (TESTING.md).
3. "Disconnected from the server" message at every `+runworld` start: cause unknown, harmless so far.
4. Transparent / alpha-tested geometry is opaque to rays (grates and fences would cast solid shadows) and
   translucent world models are not in the TLAS.
5. `GlobalDirLight` (sun) lamps are parsed but ignored; spot-lamp direction from the Euler rotation is unverified
   (no visible spot shadow has been checked against the bake).
6. Lamp shadows are hard (one ray to the lamp centre). Soft shadows need a denoised visibility buffer.
7. AO is applied in the resolve pass to the whole pixel, including translucent surfaces drawn over it.
8. Object identity is the engine object address. Slots are reused, so history can mis-associate for one frame.
9. The dynamic geometry budget is fixed (500k vertices / 1.5M indices per frame); overflow drops objects silently.
10. No parity capture against d3d.ren / d_ren has been made yet; colours and brightness are judged by eye only.

## Next steps (suggested order)

1. Parity harness: same camera, d3d.ren (dgVoodoo2) vs d_ren vs LithRay raster, diffed (TESTING.md step 4).
2. Exercise the unverified list on an outdoor level with water (sky, polygrids, particles, fog), then Alt+Tab.
3. Any-hit handling for alpha-tested surfaces in ray queries; translucent world models in the TLAS with a mask.
4. Soft lamp shadows + a proper denoiser (the motion vectors and history plumbing exist).
5. Classic-mode completeness: chrome pass, model tint, cloud shadows, LOD.
6. Fix the server speed patch; verify fullscreen and 4K performance.
