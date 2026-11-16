# Building, running and testing

## Requirements

* Visual Studio 2022 or newer with the x86 **and** x64 C++ toolsets (found through `vswhere`).
* `glslang` (default `D:\Prog\toolchains\glslang\bin\glslang.exe`, override with `-Glslang`).
* A Vulkan 1.3 GPU. Ray tracing needs `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`; without them the host
  runs the raster baseline.
* Vulkan headers, volk and VMA are vendored in `third_party/` (no Vulkan SDK needed).

## Build and deploy

```powershell
.\build.ps1 -GameDir D:\Prog\blood2_test     # build\lithray.ren (x86) + build\lithray_host.exe (x64), copied next to CLIENT.EXE
```

Both files must sit in the game directory. The game selects the renderer with `++RenderDll lithray.ren` on the
command line (or `"RenderDLL" "lithray.ren"` in `autoexec.cfg`).

## Scripted runs

```powershell
.\tools\run.ps1 -World "worlds\04_steamtunnels" -Script "waitworld","wait 14000","shot captures\a.png","quit"
.\tools\run.ps1 -World "worlds\04_steamtunnels" -Vars "lr_RT 0" -Script "waitworld","wait 14000","shot captures\raster.png","quit"
.\tools\run.ps1 -Interactive            # play; add -Windowed for a window
```

`run.ps1` backs up `autoexec.cfg` before the run and restores it afterwards (the game rewrites it on exit), starts
the game windowed in background mode (it keeps running without focus and leaves the cursor alone), and writes the
script to `lithray_cmd.txt`, which the shim polls. Script lines:

| line | effect |
|---|---|
| `waitworld` | wait until a world is loaded and 3D scenes are being rendered |
| `wait <ms>` | wait |
| `shot <file.png>` | PNG of the next presented frame (path relative to the game directory) |
| `key <name\|char>` | post a key press to the game window (`enter`, `escape`, `space`, arrows, `tab`, `f8`, …) |
| `quit` | close the game |
| anything else | run as an engine console command (`lr_RT 0`, `FogEnable 0`, …) |

Notes from real runs:

* With these arguments `+runworld <anything>` starts a **new game**: the level that loads is `01_SubwayA` (the
  intro cutscene with the train, then the carriage). Gameplay is reached about 12–14 s after `waitworld`.
* `+runworld` is ignored when the install's usual argument set (`-rez custom`, `custom\WidescreenPatch.REZ`,
  `+enableretaillevels 1`, `++CardDesc`) is used: the game stays in the main menu. Which argument causes it is not
  identified; scripted runs therefore use the stock rez set, interactive runs the install's set.
* "You have been disconnected from the server." is printed at the start of every `+runworld` session. It did not
  stop the game in any run. Whether d3d.ren shows it too is unverified.

## Two instances at once

`.\tools\run.ps1 -Dev ...` runs the binaries from `build\` as `lithray_dev.ren` with their own host, log
(`lithray_host_dev.log`) and command file, next to a deployed instance that is being played. A key press anywhere
while the title splash is up cancels `+runworld` (the game reads the keyboard globally), so a scripted run can end
up in the menu when someone is typing.

While a game started with `-Interactive` runs, commands can be sent to it by writing `lithray_cmd.txt` into the game
directory (same syntax as `-Script`): that is how captures are taken at the spot a player is standing on.

## Logs

* `lithray_shim.log` (appended): Init/Term, host start, world statistics, every model definition, rejected data.
* `lithray_host.log` (rewritten per host start): device, swapchain, world/lamp statistics, the first sighting of
  every object instance (type, size, flags, position), and every 5 s: fps, host CPU ms, GPU ms, draw / instance /
  TLAS / light counts.

`$env:LITHRAY_VALIDATE=1` (or `run.ps1 -Validate`) enables the Khronos validation layer in the host when it is
installed; messages go to the host log.

## Console variables

Saved to `autoexec.cfg` like any engine variable.

| variable | default | meaning |
|---|---|---|
| `lr_RT` | 1 | 0 = raster baseline, 1 = ray-traced lighting |
| `lr_RTShadows` | 1 | lamp and dynamic-light shadow rays |
| `lr_RTReflections` | 1 | reflection strength (0 disables) |
| `lr_RTAO` | 1 | ray-traced ambient occlusion (lightmap mode) |
| `lr_RTGI` | 1 | 1 = full ray-traced lighting (lamps + bounce + emitters, no lightmaps); 0 = lightmaps with ray-traced shadows |
| `lr_GIDirect` / `lr_GIIndirect` / `lr_GIEmissive` | 1 / 1.3 / 1 | strength of lamps, bounce light and emitters |
| `lr_GIHistory` | 0.9 | weight of the accumulated lighting history (lower = less lag, more noise) |
| `lr_VSync` | 1 | FIFO, else mailbox / immediate |
| `lr_MaxFPS` | 0 | frame cap (forced to 100 while the game-speed patch is not applied) |
| `lr_DebugView` | 0 | 1 normals, 2 lighting only, 3 ambient occlusion, 4 albedo, 5 world position, 6 one colour per instance (a screenshot then logs the instance table), 7 per surface, 8 baked-light source (r lightmapped, g vertex colour, b lightmap value), 9 surface flags / world model |
| `lr_Widescreen` | 1 | Hor+ (keep the vertical FOV), 3D uses the whole window |
| `lr_GameSpeedFix`, `lr_MouseFix` | 1 | compatibility patches (see ARCHITECTURE.md) |
| `lr_Background` | 0 | keep running without focus (test runs; set on the command line) |

## Checking a change

1. Build and deploy.
2. Run the standard capture pair (RT and `lr_RT 0`) on `01_SubwayA` at `waitworld + 14000` and at
   `waitworld + 200` (the first cutscene frame — it caught the two-sided world polygon bug).
3. Read `lithray_host.log`: no `FATAL`/`CRASH`, sensible counts, GPU time.
4. For parity questions compare with d_ren (`++RenderDll d_ren.ren`, its `d_ren_cmd.txt` `capture` command) and
   with d3d.ren under dgVoodoo2 (both are present in `D:\Prog\blood2_test`).

Reference numbers (RTX 4090, 1280×720, `01_SubwayA` carriage, ray tracing on): about 0.4 ms host CPU and 2.3 ms GPU
per frame; the game runs at its 100 fps cap.
