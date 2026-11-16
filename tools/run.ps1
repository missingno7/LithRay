# Runs Blood II with lithray.ren for a scripted or interactive test.
#   .\tools\run.ps1 -World worlds\04_steamtunnels -Script "wait 8000","shot captures\lr_test.png","quit"
#   .\tools\run.ps1 -Interactive                  (play normally; fullscreen borderless unless -Windowed)
# The game's autoexec.cfg is saved before the run and restored afterwards, so the install's own renderer choice
# and options are never changed by a test.
param(
    [string]$GameDir = "D:\Prog\blood2_test",
    [string]$World = "",
    [string[]]$Script = @(),
    [int]$Width = 1280, [int]$Height = 720,
    [switch]$Interactive,
    [switch]$Windowed,
    [string[]]$Vars = @(),          # extra "+name value" console settings, e.g. "lr_RT 0"
    [int]$TimeoutSec = 120,
    [switch]$Validate,              # Vulkan validation layer in the host (needs the Vulkan SDK)
    [switch]$Dev                    # run build\ binaries as lithray_dev.ren + build\lithray_host.exe, next to a running deployed instance
)
$ErrorActionPreference = "Stop"
$cfg = Join-Path $GameDir "autoexec.cfg"
$backup = Join-Path $GameDir "autoexec.cfg.lithray-run-backup-$PID"
$ren = "lithray.ren"
Remove-Item Env:LITHRAY_HOST, Env:LITHRAY_HOST_LOG -ErrorAction SilentlyContinue
if ($Dev) {
    $ren = "lithray_dev.ren"
    Copy-Item (Join-Path $PSScriptRoot "..\build\lithray.ren") (Join-Path $GameDir $ren) -Force
    $env:LITHRAY_HOST = (Resolve-Path (Join-Path $PSScriptRoot "..\build\lithray_host.exe")).Path
    $env:LITHRAY_HOST_LOG = "lithray_host_dev.log"
}
$cmdName = if ($Dev) { "lithray_cmd_dev.txt" } else { "lithray_cmd.txt" }
if ($Dev) { $env:LITHRAY_CMD = $cmdName } else { Remove-Item Env:LITHRAY_CMD -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $GameDir $cmdName) -ErrorAction SilentlyContinue
if ($Script.Count) { Set-Content (Join-Path $GameDir $cmdName) $Script -Encoding ascii }
Copy-Item $cfg $backup -Force


# Scripted runs use the stock rez set: with the install's custom rez / widescreen patch arguments +runworld is ignored
# (cause not yet identified, see docs/TESTING.md). Interactive runs use the install's usual argument set.
$a = @('-windowtitle', '"Blood2: The Chosen"', '-rez', 'BLOOD2.REZ', '-rez', 'SOUND.REZ', '-rez', 'BLOOD2P.REZ', '-rez', 'BLOOD2P2.REZ',
    '+multiplayer', '0', '++RenderDll', $ren, '++screenwidth', $Width, '++screenheight', $Height)
if ($Interactive) {
    $a = @('-windowtitle', '"Blood2: The Chosen"', '-rez', 'Blood2.rez', '-rez', 'sound.rez', '-rez', 'custom', '-rez', 'Blood2p.rez', '-rez', 'Blood2p2.rez',
        '+multiplayer', '0', '++RenderDll', $ren, '++CardDesc', 'primary', '++screenwidth', $Width, '++screenheight', $Height, '++screendepth', '16',
        '-rez', 'custom\WidescreenPatch.REZ', '+enableretaillevels', '1')
}
if (-not $Interactive) { $a += @('+lr_Background', '1', '+windowed', '1') }
elseif ($Windowed) { $a += @('+windowed', '1') }
foreach ($v in $Vars) { $p = $v.Split(' ', 2); $a += @("+$($p[0])", $p[1]) }
if ($World) { $a += @('+runworld', $World) }
if ($Validate) { $env:LITHRAY_VALIDATE = "1" } else { Remove-Item Env:LITHRAY_VALIDATE -ErrorAction SilentlyContinue }

try {
    $p = Start-Process (Join-Path $GameDir "CLIENT.EXE") -ArgumentList $a -WorkingDirectory $GameDir -PassThru
    if ($Interactive) { $p.WaitForExit() }
    elseif (-not $p.WaitForExit($TimeoutSec * 1000)) {
        Write-Host "timeout after $TimeoutSec s: closing the game"
        $p.CloseMainWindow() | Out-Null
        if (-not $p.WaitForExit(5000)) { $p.Kill() }
    }
    Write-Host "game exit code: $($p.ExitCode)"
}
finally {
    Start-Sleep -Milliseconds 300
    $hostDir = if ($Dev) { (Resolve-Path (Join-Path $PSScriptRoot "..\build")).Path } else { $GameDir }
    Get-Process lithray_host -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$hostDir*" } | ForEach-Object { $_.WaitForExit(3000) | Out-Null; if (-not $_.HasExited) { $_.Kill() } }
    Copy-Item $backup $cfg -Force
    Remove-Item $backup -Force
}
