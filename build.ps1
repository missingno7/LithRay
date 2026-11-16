# Builds LithRay: shaders -> SPIR-V headers, lithray.ren (x86), lithray_host.exe (x64).
#   .\build.ps1                      build into build\
#   .\build.ps1 -GameDir D:\Prog\blood2_test    also deploy next to CLIENT.EXE
#   .\build.ps1 -Debug               unoptimised with debug info
param(
    [string]$GameDir = "",
    [switch]$Debug,
    [string]$Glslang = "D:\Prog\toolchains\glslang\bin\glslang.exe"
)
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out, "$out\x86", "$out\x64", "$out\gen" | Out-Null

if (-not (Test-Path $Glslang)) { $Glslang = (Get-Command glslang, glslangValidator -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
if (-not $Glslang) { throw "glslang not found (pass -Glslang)" }

# ---- shaders: each stage becomes a C array in build\gen\<name>.h, collected by shaders.h
$shaders = @(
    @{ src = "scene.vert"; name = "scene_vert_spv"; defs = @() },
    @{ src = "scene.frag"; name = "scene_frag_spv"; defs = @() },
    @{ src = "scene.frag"; name = "scene_rt_frag_spv"; defs = @("-DRT") },
    @{ src = "post.vert"; name = "post_vert_spv"; defs = @() },
    @{ src = "post.frag"; name = "post_frag_spv"; defs = @() }
)
$newest = (Get-ChildItem "$root\shaders" | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime
$include = ""
foreach ($s in $shaders) {
    $dst = "$out\gen\$($s.name).h"
    if (-not (Test-Path $dst) -or (Get-Item $dst).LastWriteTime -lt $newest) {
        & $Glslang -V --target-env vulkan1.3 "-I$root\shaders" @($s.defs) --vn $s.name -o $dst "$root\shaders\$($s.src)" | Out-Null
        if ($LASTEXITCODE) { & $Glslang -V --target-env vulkan1.3 "-I$root\shaders" @($s.defs) "$root\shaders\$($s.src)" -o "$out\gen\err.spv"; throw "shader $($s.src) failed" }
    }
    $include += "#include `"$($s.name).h`"`n"
}
Set-Content "$out\gen\shaders.h" "#pragma once`n#include <stdint.h>`n$include" -Encoding ascii

# ---- C++ (MSVC). vcvars is imported per architecture.
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
if (-not $vs) { throw "Visual Studio not found" }
$vcvars = "$vs\VC\Auxiliary\Build\vcvarsall.bat"
$opt = if ($Debug) { "/Od /Zi /MTd" } else { "/O2 /Zi /MT /DNDEBUG" }
$common = "/nologo /std:c++20 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX $opt"

function Invoke-Cl([string]$arch, [string]$cmdline) {
    cmd /c "`"$vcvars`" $arch >nul 2>&1 && $cmdline"
    if ($LASTEXITCODE) { throw "build failed ($arch)" }
}

Invoke-Cl "x86" "cl $common /Fo`"$out\x86\\`" /Fd`"$out\x86\lithray_ren.pdb`" `"$root\shim\shim.cpp`" `"$root\shim\scene.cpp`" `"$root\shim\bridge.cpp`" /LD /Fe`"$out\lithray.ren`" /link /DEBUG /PDB:`"$out\lithray_ren.pdb`" /IMPLIB:`"$out\x86\lithray.lib`" user32.lib winmm.lib"

$inc = "/I`"$root\third_party\Vulkan-Headers\include`" /I`"$root\third_party\volk`" /I`"$root\third_party\vma\include`" /I`"$out\gen`""
# vk_mem_alloc/volk live in gfx.cpp and rarely change: it is compiled separately so iteration on the renderer is fast
$gfxObj = "$out\x64\gfx.obj"
if (-not (Test-Path $gfxObj) -or (Get-Item $gfxObj).LastWriteTime -lt (Get-Item "$root\host\gfx.cpp").LastWriteTime -or (Get-Item $gfxObj).LastWriteTime -lt (Get-Item "$root\host\gfx.h").LastWriteTime) {
    Invoke-Cl "x64" "cl $common $inc /c /Fo`"$gfxObj`" /Fd`"$out\x64\host.pdb`" `"$root\host\gfx.cpp`""
}
Invoke-Cl "x64" "cl $common $inc /Fo`"$out\x64\\`" /Fd`"$out\x64\host.pdb`" `"$root\host\main.cpp`" `"$root\host\renderer.cpp`" `"$root\host\effects.cpp`" `"$root\host\lamps.cpp`" `"$gfxObj`" /Fe`"$out\lithray_host.exe`" /link /DEBUG /PDB:`"$out\lithray_host.pdb`" /SUBSYSTEM:CONSOLE user32.lib"

Write-Host "built $out\lithray.ren and $out\lithray_host.exe"
if ($GameDir) {
    Copy-Item "$out\lithray.ren", "$out\lithray_host.exe", "$out\lithray_ren.pdb", "$out\lithray_host.pdb" $GameDir -Force
    Write-Host "deployed to $GameDir"
}
