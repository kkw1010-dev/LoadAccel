# Shared by Build.ps1 and Test.ps1: where the toolchain is. Machine-specific values come from tools\local.ps1
# (not part of the public repository) or from the environment; nothing here names a local path.
#   VCPKG_ROOT      vcpkg checkout (CMakePresets.json uses it)
#   COMMONLIB_DIR   CommonLibSSE-NG checkout (alandtse/CommonLibVR, branch ng)
#   LOADACCEL_IMAGE optional: a memory image of SkyrimSE.exe 1.6.1170 for the constants check
#   LOADACCEL_IMAGE_SE optional: SkyrimSE.exe 1.5.97 without the Steam stub (Steamless output) for the same check
#   $PrivacyWords   extra strings that must not appear in the DLL
$PrivacyWords = @()
$local = Join-Path $PSScriptRoot 'local.ps1'
if (Test-Path $local) { . $local }

$VsDevCmd = $env:LOADACCEL_VSDEVCMD
if (-not $VsDevCmd) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) { $VsDevCmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat' }
    }
}
if (-not $VsDevCmd -or -not (Test-Path $VsDevCmd)) { throw 'Visual Studio C++ build tools not found (vswhere), and LOADACCEL_VSDEVCMD is not set.' }
