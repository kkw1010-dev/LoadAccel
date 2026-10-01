<#
.SYNOPSIS
  Build and run the offline tests (no game, no CommonLib): the index cores against models of the engine
  functions. Exit code 0 only when every test program passed.
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\Test.ps1
#>
$ErrorActionPreference = 'Stop'
$Repo     = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Env.ps1')
$Out = Join-Path $Repo 'build\test'
New-Item -ItemType Directory -Force $Out | Out-Null

$env:VSCMD_SKIP_SENDTELEMETRY = '1'
$before = @(Get-Process -Name mspdbsrv, vctip -ErrorAction SilentlyContinue | ForEach-Object Id)
$failed = @()
foreach ($source in Get-ChildItem (Join-Path $Repo 'tests') -Filter '*_test.cpp') {
    $exe = Join-Path $Out ($source.BaseName + '.exe')
    $cmd = "`"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul && cl /nologo /std:c++latest /EHsc /W4 /WX /O2 /Fo`"$Out\\`" /Fe`"$exe`" `"$($source.FullName)`""
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { $failed += "$($source.Name): compile"; continue }
    & $exe
    if ($LASTEXITCODE -ne 0) { $failed += "$($source.Name): exit $LASTEXITCODE" }
}
$left = @(Get-Process -Name mspdbsrv, vctip -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
if ($left) { $left | Stop-Process -Force }
if ($failed) { throw ("TESTS FAILED: " + ($failed -join '; ')) }
Write-Host 'ALL TESTS PASSED'
