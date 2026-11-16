# LithRay — working notes for Claude

Read `docs/ARCHITECTURE.md`, `docs/STATUS.md` and `docs/TESTING.md` first; keep STATUS.md current when something
is verified, found broken, or decided.

* Build + deploy: `.\build.ps1 -GameDir D:\Prog\blood2_test` (PowerShell). Run: `.\tools\run.ps1` (see TESTING.md).
  Always go through `run.ps1`: it restores the game's `autoexec.cfg`, which belongs to the user's d_ren setup.
* Validate by running the real game and reading the PNG captures and `lithray_host.log`; the first cutscene frame
  (`waitworld` + 200 ms) and gameplay (`waitworld` + 14000 ms) are the standard shots.
* Engine facts come from `D:\Prog\blood2_recon` (`docs/renderer_closure/*.md`, `recon/renderer/include/lt1_port_abi.h`,
  SDK headers in `assets/blood2src/AppHeaders`) and from d_ren (`D:\Prog\LithTech-1-Renderer\source`). Look constants
  and semantics up there; do not write them from memory.
* The shim must stay thin and must never keep an engine pointer past a slot call. The host must never see one.
  Any change to `common/protocol.h` bumps `kVersion`.
* Anything that reads engine memory in the shim runs under SEH (`__try`); a bad read must not take the game down.
