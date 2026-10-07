<#
.SYNOPSIS
  Build LoadAccel.dll with the given built-in stages, after the offline tests pass, and put it under
  build\dist\A<Stage>-B<StageB>\ (A = source-file lists, B = large references).
  Does not install anything.
.DESCRIPTION
  Needs Visual Studio C++ build tools, CMake, Ninja, and the environment variables VCPKG_ROOT and COMMONLIB_DIR
  (see tools\Env.ps1). The build folder is build\work (preset "work"). build\dist\A<N>-B<M>\LoadAccel.dll is the
  result; its SHA-256 is printed and written next to it.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\Build.ps1 -Stage 2 -StageB 1
#>
param([int]$Stage = 2, [int]$StageB = 0, [string]$Preset = 'work', [switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$Repo     = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Env.ps1')
if (-not $env:VCPKG_ROOT -or -not $env:COMMONLIB_DIR) { throw 'Set VCPKG_ROOT and COMMONLIB_DIR (see tools\Env.ps1).' }

if (-not $SkipTests) {
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'Test.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Offline tests failed: no DLL is built from code that fails them.' }
}

# Every engine constant in the sources must match the images the analysis was done on: 1.6.1170 (LOADACCEL_IMAGE)
# and 1.5.97 (LOADACCEL_IMAGE_SE).
$verify = Join-Path $Repo 'research\verify_constants.py'
$images = @()
if ($env:LOADACCEL_IMAGE -and (Test-Path $env:LOADACCEL_IMAGE)) { $images += @('--ae', $env:LOADACCEL_IMAGE) }
if ($env:LOADACCEL_IMAGE_SE -and (Test-Path $env:LOADACCEL_IMAGE_SE)) { $images += @('--se', $env:LOADACCEL_IMAGE_SE) }
if ($images -and (Test-Path $verify)) {
    python $verify @images
    if ($LASTEXITCODE -ne 0) { throw 'A constant in the sources does not match an image: nothing is built.' }
    if ($images.Count -lt 4) { Write-Host 'NOTE: only one runtime image given (LOADACCEL_IMAGE / LOADACCEL_IMAGE_SE): the other runtime''s constants were not re-checked here.' }
} else {
    Write-Host 'NOTE: no image of SkyrimSE.exe given (LOADACCEL_IMAGE, LOADACCEL_IMAGE_SE): the constants were not re-checked here. The DLL checks the code it relies on at load anyway.'
}

# No build servers left behind: Ninja, embedded debug info, no telemetry; stop only what this build started.
$env:MSBUILDDISABLENODEREUSE = '1'
$env:VSCMD_SKIP_SENDTELEMETRY = '1'
$env:VCPKG_DISABLE_METRICS = '1'
$before = @(Get-Process -Name mspdbsrv, vctip, MSBuild -ErrorAction SilentlyContinue | ForEach-Object Id)

$cmd = "`"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && cd /d `"$Repo`" && cmake --preset $Preset -DLOADACCEL_STAGE=$Stage -DLOADACCEL_B_STAGE=$StageB && cmake --build --preset $Preset"
cmd /c $cmd
$buildExit = $LASTEXITCODE
$left = @(Get-Process -Name mspdbsrv, vctip, MSBuild -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
if ($left) {
    Write-Host ("stopping build leftovers: " + (($left | ForEach-Object { "$($_.Name)($($_.Id))" }) -join ', '))
    $left | Stop-Process -Force
}
if ($buildExit -ne 0) { throw "Build failed (exit $buildExit)." }

$dll = Join-Path $Repo "build\$Preset\LoadAccel.dll"
if (-not (Test-Path $dll)) { throw "Build reported success but $dll is missing." }
# The DLL may be handed to testers: no local path, user name or mail address may be in it.
$bytes = [System.IO.File]::ReadAllBytes($dll)
$latin = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
$wide  = [System.Text.Encoding]::Unicode.GetString($bytes)
$found = (@('Users\', 'Users/', $env:USERNAME) + $PrivacyWords) |
    Where-Object { $_ -and ($latin.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $wide.IndexOf($_, [StringComparison]::OrdinalIgnoreCase) -ge 0) }
if ($found) { throw ("PRIVACY CHECK FAILED: the DLL contains " + ($found -join ', ') + ". Nothing was copied to build\dist.") }
Write-Host 'privacy check: no local path, user name or mail address in the DLL'

$dist = Join-Path $Repo "build\dist\A$Stage-B$StageB"
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $dll (Join-Path $dist 'LoadAccel.dll') -Force
$hash = (Get-FileHash (Join-Path $dist 'LoadAccel.dll') -Algorithm SHA256).Hash
Set-Content -Path (Join-Path $dist 'LoadAccel.sha256') -Value $hash -Encoding ascii
Write-Host "built: $dist\LoadAccel.dll (source-file lists stage $Stage, large refs stage $StageB, SHA-256 $hash)"
