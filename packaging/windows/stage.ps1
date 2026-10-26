# File/layer: packaging/windows/stage.ps1 | release packaging, Windows x64
# Testing: run by .github/workflows/release.yml on every release tag, and
#          by hand against a local build (docs/development.md, "Releases").
#
# Stages the Windows release from a built tree, checks it, and zips it:
#   dist/qttune-<version>-windows-x64/       the folder a user unzips
#   dist/qttune-<version>-windows-x64.zip
#   dist/SHA256SUMS.txt
#
# The checks are what a user without Visual Studio or Qt would hit first:
#   - both surrogates are there, the x86 one and the x64 one, since every
#     vendor DLL runs in the one of its architecture (ADR-0025), and
#     neither imports a C runtime DLL (each links it statically; an x86
#     vcruntime140.dll can't sit beside the app's x64 one);
#   - every DLL qttune.exe imports is in the folder or is part of Windows;
#   - no QML debugging: qttune.exe doesn't import QQmlDebuggingEnabler
#     (QT_QML_DEBUG, Debug builds only) and no qmltooling plugins ship;
#   - the staged qttune.exe starts and is still running after -SmokeSeconds,
#     which a missing Qt plugin or QML module would stop at once.
#
# Usage: pwsh packaging/windows/stage.ps1 -Version 0.3.1 [-BuildDir build]
#        [-OutDir dist] [-SmokeSeconds 10]

param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $BuildDir = 'build',
    [string] $OutDir = 'dist',
    [int] $SmokeSeconds = 10
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$appDir = Join-Path $repo "$BuildDir\app\Release"
$name = "qttune-$Version-windows-x64"
$out = Join-Path $repo $OutDir
$stage = Join-Path $out $name

function Fail([string] $why) { Write-Error "stage: $why"; exit 1 }

if (-not (Test-Path (Join-Path $appDir 'qttune.exe'))) { Fail "no qttune.exe in $appDir; build the app first" }
if (-not (Test-Path (Join-Path $appDir 'ecuio-j2534-surrogate.exe'))) {
    Fail 'no x86 surrogate beside qttune.exe; build build-win32 before configuring the app (CLAUDE.md, Commands)'
}
if (-not (Test-Path (Join-Path $appDir 'ecuio-j2534-surrogate-x64.exe'))) {
    Fail 'no x64 surrogate beside qttune.exe; the app build copies it from its own tree (app/CMakeLists.txt)'
}

# ---- Visual Studio: the redistributable CRT and dumpbin
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'no Visual Studio with the C++ x64 tools' }
$redist = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version] $_.Name } | Select-Object -Last 1
$crt = Get-ChildItem (Join-Path $redist.FullName 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' | Select-Object -First 1
if (-not $crt) { Fail "no x64 CRT under $($redist.FullName)" }
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version] $_.Name } | Select-Object -Last 1 |
    ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' }

function Imports([string] $exe) {
    # dumpbin lists one imported DLL per line, indented, under "dependencies:"
    & $dumpbin /nologo /dependents $exe |
        Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } |
        ForEach-Object { $Matches[1] }
}

# ---- Stage
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
# The deployed app as windeployqt left it, minus the link-time leftovers
Copy-Item -Recurse (Join-Path $appDir '*') $stage
Get-ChildItem $stage -Recurse -Include *.pdb, *.lib, *.exp, *.ilk | Remove-Item -Force
# The app-local CRT: windeployqt runs with --no-compiler-runtime
Copy-Item (Join-Path $crt.FullName '*.dll') $stage
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $repo 'DISCLAIMER.md') $stage
Copy-Item (Join-Path $PSScriptRoot 'README.txt') $stage
Copy-Item (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') $stage
Copy-Item -Recurse (Join-Path $PSScriptRoot 'licenses') $stage
Write-Host "staged $name with the CRT from $($crt.FullName)"

# ---- Check: each surrogate carries its own CRT
foreach ($sur in 'ecuio-j2534-surrogate.exe', 'ecuio-j2534-surrogate-x64.exe') {
    $surImports = Imports (Join-Path $stage $sur)
    $crtImports = $surImports | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt)' }
    if ($crtImports) { Fail "$sur imports the C runtime: $($crtImports -join ', ')" }
    Write-Host "$sur imports: $($surImports -join ', ')"
}

# ---- Check: everything qttune.exe imports is here or is Windows'
$system = Join-Path $env:SystemRoot 'System32'
foreach ($dll in Imports (Join-Path $stage 'qttune.exe')) {
    if ($dll -match '^(api|ext)-ms-') { continue }   # API sets resolve inside Windows
    if (Test-Path (Join-Path $stage $dll)) { continue }
    if (Test-Path (Join-Path $system $dll)) { continue }
    Fail "qttune.exe imports $dll, which is neither in the package nor part of Windows"
}
Write-Host 'qttune.exe imports: all present'

# ---- Check: no QML debugging. A build with it opens a debug server that
# can run script in the app when launched with -qmljsdebugger.
$debugImport = & $dumpbin /nologo /imports (Join-Path $stage 'qttune.exe') |
    Select-String -SimpleMatch 'QQmlDebuggingEnabler'
if ($debugImport) { Fail 'qttune.exe was built with QT_QML_DEBUG (app/CMakeLists.txt: Debug builds only)' }
if (Test-Path (Join-Path $stage 'qmltooling')) {
    Fail "qmltooling is staged: an older deploy left it in $appDir; delete it and build again"
}
Write-Host 'QML debugging: none'

# ---- Check: the staged app starts and stays up
if ($SmokeSeconds -gt 0) {
    $p = Start-Process -FilePath (Join-Path $stage 'qttune.exe') -WorkingDirectory $stage -PassThru
    Start-Sleep -Seconds $SmokeSeconds
    if ($p.HasExited) { Fail "the staged qttune.exe exited within $SmokeSeconds s (code $($p.ExitCode))" }
    Stop-Process -Id $p.Id -Force
    Write-Host "smoke: qttune.exe ran $SmokeSeconds s"
}

# ---- Zip and checksum
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content -Encoding ascii (Join-Path $out 'SHA256SUMS.txt')
Write-Host "$name.zip $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, sha256 $hash"
