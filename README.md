# LithRay

A native x64 Vulkan renderer with hardware ray tracing for **Blood II: The Chosen** (LithTech 1.0), running with
the original, unmodified 32-bit game.

* `lithray.ren` — a thin 32-bit renderer plugin loaded by `CLIENT.EXE`. It implements the engine's renderer ABI,
  snapshots what the engine exposes and forwards it over shared memory.
* `lithray_host.exe` — the 64-bit renderer process: scene store, rasterised baseline, ray-traced lamp shadows,
  ambient occlusion and reflections.

```powershell
.\build.ps1 -GameDir D:\Prog\blood2_test
.\tools\run.ps1 -Interactive -Windowed
```

* [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the bridge, ownership, presentation, scene representation.
* [docs/STATUS.md](docs/STATUS.md) — what is verified, what is not, known deviations, open problems, next steps.
* [docs/TESTING.md](docs/TESTING.md) — build, scripted runs, captures, console variables.

Layout: `common/` protocol, `shim/` x86 plugin, `host/` x64 renderer, `shaders/` GLSL, `tools/` scripts,
`third_party/` Vulkan headers, volk, VMA.

References (not part of this repository): `D:\Prog\blood2_recon` (reconstruction: ABI and semantics) and
`D:\Prog\LithTech-1-Renderer` (the existing 32-bit Vulkan renderer).
