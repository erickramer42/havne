# File/layer: packaging/windows/stage.ps1 | release packaging, Windows x64
# Testing: run by .github/workflows/release.yml on every release tag, and
#          by hand against a local build (docs/development.md, "Releases").
#
# Stages the Windows release from a built tree, checks it, and zips it:
#   dist/havne-<version>-windows-x64/       the folder a user unzips
#   dist/havne-<version>-windows-x64.zip
#   dist/havne-<version>-windows-x64-symbols.zip   the four PDBs (ADR-0034)
#   dist/havne-<version>-windows-x64.zip.components.json   for the SBOM (ADR-0035)
#   dist/SHA256SUMS.txt
#
# The checks are what a user without Visual Studio or Qt would hit first:
#   - both surrogates are there, the x86 one and the x64 one, since every
#     vendor DLL runs in the one of its architecture (ADR-0025), and
#     neither imports a C runtime DLL (each links it statically; an x86
#     vcruntime140.dll can't sit beside the app's x64 one);
#   - every DLL havne.exe and havne-cli.exe import is in the folder or is
#     part of Windows;
#   - havne.exe, havne-cli.exe and both surrogates carry this release's
#     version in their version resource (Explorer's Details; code signing
#     reads it);
#   - no QML debugging: havne.exe doesn't import QQmlDebuggingEnabler
#     (QT_QML_DEBUG, Debug builds only) and no qmltooling plugins ship;
#   - each of the four executables has its PDB, which matches it (dumpbin
#     finds it beside the exe, signature and age checked), and records
#     only the PDB's file name, never the build's path: release.yml builds
#     with LINK=/PDBALTPATH:%_PDB%, since CMake's Visual Studio generator
#     can't pass a % through (CMake issue 15865);
#   - the staged havne.exe starts and is still running after -SmokeSeconds,
#     which a missing Qt plugin or QML module would stop at once, and the
#     staged havne-cli.exe streams a few mock:// frames and ends with
#     exit 0 (ADR-0031).
#
# Usage: pwsh packaging/windows/stage.ps1 -Version 0.3.1 [-BuildDir build]
#        [-Win32BuildDir build-win32] [-OutDir dist] [-SmokeSeconds 10]
#        [-AllowBuildPath]
#
# -AllowBuildPath: a local dry run of trees built without LINK set (the
# release skill's check, after scripts/verify.sh): an exe may record its
# PDB's full build path, as long as the file name is right and the PDB
# matches. Never for a published release.

param(
    [Parameter(Mandatory)] [string] $Version,
    [string] $BuildDir = 'build',
    [string] $Win32BuildDir = 'build-win32',
    [string] $OutDir = 'dist',
    [int] $SmokeSeconds = 10,
    [switch] $AllowBuildPath
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$appDir = Join-Path $repo "$BuildDir\app\Release"
$name = "havne-$Version-windows-x64"
$out = Join-Path $repo $OutDir
$stage = Join-Path $out $name

function Fail([string] $why) { Write-Error "stage: $why"; exit 1 }

if (-not (Test-Path (Join-Path $appDir 'havne.exe'))) { Fail "no havne.exe in $appDir; build the app first" }
if (-not (Test-Path (Join-Path $appDir 'ecuio-j2534-surrogate.exe'))) {
    Fail 'no x86 surrogate beside havne.exe; build build-win32 before configuring the app (CLAUDE.md, Commands)'
}
if (-not (Test-Path (Join-Path $appDir 'ecuio-j2534-surrogate-x64.exe'))) {
    Fail 'no x64 surrogate beside havne.exe; the app build copies it from its own tree (app/CMakeLists.txt)'
}
if (-not (Test-Path (Join-Path $appDir 'havne-cli.exe'))) {
    Fail 'no havne-cli.exe beside havne.exe; the app build copies it from cli/ (app/CMakeLists.txt)'
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

# ---- Check: everything havne.exe and havne-cli.exe import is here or is Windows'
$system = Join-Path $env:SystemRoot 'System32'
foreach ($exe in 'havne.exe', 'havne-cli.exe') {
    foreach ($dll in Imports (Join-Path $stage $exe)) {
        if ($dll -match '^(api|ext)-ms-') { continue }   # API sets resolve inside Windows
        if (Test-Path (Join-Path $stage $dll)) { continue }
        if (Test-Path (Join-Path $system $dll)) { continue }
        Fail "$exe imports $dll, which is neither in the package nor part of Windows"
    }
    Write-Host "$exe imports: all present"
}

# ---- Check: each exe's version resource is this release's version
$want = $Version -replace '-.*$', ''   # 0.3.2-rc1 and 0.3.2-local carry 0.3.2
foreach ($exe in 'havne.exe', 'havne-cli.exe', 'ecuio-j2534-surrogate.exe', 'ecuio-j2534-surrogate-x64.exe') {
    $info = (Get-Item (Join-Path $stage $exe)).VersionInfo
    if ($info.ProductVersion -ne $want -or $info.ProductName -ne 'Havne') {
        Fail "$exe carries '$($info.ProductName) $($info.ProductVersion)', not 'Havne $want'; rebuild both trees after the version changes"
    }
}
Write-Host "version resources: Havne $want"

# ---- Check: no QML debugging. A build with it opens a debug server that
# can run script in the app when launched with -qmljsdebugger.
$debugImport = & $dumpbin /nologo /imports (Join-Path $stage 'havne.exe') |
    Select-String -SimpleMatch 'QQmlDebuggingEnabler'
if ($debugImport) { Fail 'havne.exe was built with QT_QML_DEBUG (app/CMakeLists.txt: Debug builds only)' }
if (Test-Path (Join-Path $stage 'qmltooling')) {
    Fail "qmltooling is staged: an older deploy left it in $appDir; delete it and build again"
}
Write-Host 'QML debugging: none'

# ---- Symbols (ADR-0034): each executable's PDB, from the tree that built
# it (the app build copies only the .exe beside havne.exe)
$pdbs = [ordered]@{
    'havne.exe'                     = Join-Path $repo "$BuildDir\app\Release\havne.pdb"
    'havne-cli.exe'                 = Join-Path $repo "$BuildDir\cli\Release\havne-cli.pdb"
    'ecuio-j2534-surrogate-x64.exe' = Join-Path $repo "$BuildDir\surrogate\Release\ecuio-j2534-surrogate-x64.pdb"
    'ecuio-j2534-surrogate.exe'     = Join-Path $repo "$Win32BuildDir\surrogate\Release\ecuio-j2534-surrogate.pdb"
}
$symName = "$name-symbols"
$symStage = Join-Path $out $symName
if (Test-Path $symStage) { Remove-Item -Recurse -Force $symStage }
New-Item -ItemType Directory -Force $symStage | Out-Null
$check = Join-Path $out 'pdb-check'
foreach ($exe in $pdbs.Keys) {
    $pdb = $pdbs[$exe]
    $pdbName = [IO.Path]::GetFileName($pdb)
    if (-not (Test-Path $pdb)) { Fail "no $pdbName for $exe at $pdb; a Release build writes one (CMakeLists.txt, ADR-0034)" }
    # The debug directory names the PDB: its file name only
    # ("Format: RSDS, {GUID}, age, <recorded path>")
    $rsds = & $dumpbin /nologo /headers (Join-Path $stage $exe) |
        Select-String -Pattern 'Format: RSDS, \{[^}]+\}, \d+, (.*)$' | Select-Object -First 1
    $recorded = if ($rsds) { $rsds.Matches[0].Groups[1].Value.Trim() } else { '' }
    $buildPath = $AllowBuildPath -and [IO.Path]::GetFileName($recorded) -eq $pdbName
    if ($recorded -ne $pdbName -and -not $buildPath) {
        Fail "$exe records its PDB as '$recorded', not '$pdbName'; build with LINK=/PDBALTPATH:%_PDB% in the environment (release.yml does)"
    }
    # The shipped exe and its PDB, alone in a folder: dumpbin finds the PDB
    # only if its signature and age are the exe's. An exe that records its
    # build path is found there first, the same file
    if (Test-Path $check) { Remove-Item -Recurse -Force $check }
    New-Item -ItemType Directory -Force $check | Out-Null
    Copy-Item (Join-Path $stage $exe) $check
    Copy-Item $pdb $check
    $at = @(Join-Path $check $pdbName)
    if ($buildPath) { $at += (Resolve-Path $pdb).Path }
    $found = & $dumpbin /nologo /pdbpath:verbose (Join-Path $check $exe) |
        Select-String -Pattern 'PDB file found at ''(.*)''' |
        Where-Object { $at -contains $_.Matches[0].Groups[1].Value }
    if (-not $found) { Fail "$pdbName doesn't match the staged $exe; rebuild both trees" }
    Copy-Item $pdb $symStage
}
Remove-Item -Recurse -Force $check
Write-Host "symbols: $($pdbs.Count) PDBs, each matching its exe"

# ---- Check: the staged app starts and stays up
if ($SmokeSeconds -gt 0) {
    $p = Start-Process -FilePath (Join-Path $stage 'havne.exe') -WorkingDirectory $stage -PassThru
    Start-Sleep -Seconds $SmokeSeconds
    if ($p.HasExited) { Fail "the staged havne.exe exited within $SmokeSeconds s (code $($p.ExitCode))" }
    Stop-Process -Id $p.Id -Force
    Write-Host "smoke: havne.exe ran $SmokeSeconds s"
}
# The CLI needs no desktop: a few frames, an end line, exit 0
$cliOut = & (Join-Path $stage 'havne-cli.exe') stream mock://stage --count 5
if ($LASTEXITCODE -ne 0) { Fail "the staged havne-cli.exe exited $LASTEXITCODE on mock://stage" }
$cliEnd = $cliOut | Select-Object -Last 1
if ($cliEnd -notmatch '^\{"type":"end","reason":"count","frames":5,') {
    Fail "the staged havne-cli.exe didn't end its mock:// stream: $cliEnd"
}
Write-Host 'smoke: havne-cli.exe streamed 5 mock:// frames'

# ---- The component list for the release's SBOM (packaging/sbom.py,
# ADR-0035 §5): every DLL in the folder belongs to a component, by name,
# with the version its own version resource carries; one that doesn't
# fails the release rather than go unlisted. Havne's executables are
# sbom.py's to add.
# The product version's first word ("1.8.2502.11 (239921522)"); none
# for a DLL without a version resource (opengl32sw.dll)
function DllVersion([string] $path) {
    $v = (Get-Item $path).VersionInfo.ProductVersion
    if ($v) { ($v.Trim() -split '\s+')[0] } else { $null }
}
$groups = [ordered]@{
    'Qt'                                 = @{ match = '^Qt6.*\.dll$'; supplier = 'Organization: The Qt Company'; license = 'LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only'; versionOf = 'Qt6Core.dll' }
    'Microsoft Visual C++ runtime'       = @{ match = '^(vcruntime|msvcp|concrt|vccorlib)\d+.*\.dll$'; supplier = 'Organization: Microsoft'; versionOf = 'vcruntime140.dll'; comment = 'app-local' }
    'Microsoft D3DCompiler'              = @{ match = '^D3Dcompiler_47\.dll$'; supplier = 'Organization: Microsoft'; versionOf = 'D3Dcompiler_47.dll' }
    'Microsoft DirectX Shader Compiler'  = @{ match = '^(dxcompiler|dxil)\.dll$'; supplier = 'Organization: Microsoft'; versionOf = 'dxcompiler.dll' }
    'Mesa llvmpipe'                      = @{ match = '^opengl32sw\.dll$'; versionOf = 'opengl32sw.dll'; comment = 'opengl32sw.dll, the software OpenGL fallback windeployqt deploys' }
}
$files = @{}
foreach ($k in $groups.Keys) { $files[$k] = [Collections.Generic.List[string]]::new() }
foreach ($dll in Get-ChildItem $stage -Recurse -Filter *.dll) {
    $rel = $dll.FullName.Substring($stage.Length + 1).Replace('\', '/')
    $owner = $null
    foreach ($k in $groups.Keys) {
        # A DLL in a subfolder (plugins, QML modules) is windeployqt's Qt
        if ($rel -match $groups[$k].match -or ($k -eq 'Qt' -and $rel.Contains('/'))) { $owner = $k; break }
    }
    if (-not $owner) { Fail "$rel belongs to no SBOM component: add it to stage.ps1's list" }
    $files[$owner].Add($rel)
}
$components = foreach ($k in $groups.Keys) {
    if ($files[$k].Count -eq 0) { continue }
    $g = $groups[$k]
    $c = [ordered]@{ name = $k; files = @($files[$k] | Sort-Object) }
    $version = DllVersion (Join-Path $stage $g.versionOf)
    if ($k -eq 'Qt') { $version = ($version -split '\.')[0..2] -join '.' }   # 6.12.0.0
    if ($version) { $c.version = $version }
    if ($g.supplier) { $c.supplier = $g.supplier }
    if ($g.license) { $c.license = $g.license }
    if ($g.comment) { $c.comment = $g.comment }
    if ($k -eq 'Qt') { $c.purl = "pkg:generic/qt@$($c.version)" }
    $c
}
$listing = [ordered]@{ download = "$name.zip"; purpose = 'APPLICATION'; components = @($components) }
[IO.File]::WriteAllText((Join-Path $out "$name.zip.components.json"), ($listing | ConvertTo-Json -Depth 5) + "`n")
Write-Host "components: $(@($components).Count) listed for the SBOM"

# ---- Zip and checksum
$sums = ''
foreach ($folder in $name, $symName) {
    $zip = Join-Path $out "$folder.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $out $folder) -DestinationPath $zip
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
    $sums += "$hash  $folder.zip`n"
    Write-Host "$folder.zip $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB, sha256 $hash"
}
# LF, not Set-Content's CRLF: sha256sum -c reads the CR as part of the name
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), $sums)
