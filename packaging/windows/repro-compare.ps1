# File/layer: packaging/windows/repro-compare.ps1 | PowerShell 7, release engineering
# Testing: repro-windows.yml (CI); run by hand over two trees built from one commit
#
# Compares two Windows builds of one commit (ADR-0035): every .exe and
# .dll under each tree's build and build-win32 folders must match byte for
# byte, which gates; the release zip and the symbols zip (its PDBs record
# the build's paths) are reported, and the zip becomes a gate once it has
# held over several runs.
#
# Usage: pwsh packaging/windows/repro-compare.ps1 <rootA> <rootB>
# Each root holds build\, build-win32\ and dist\ (the staged zips).

param(
    [Parameter(Mandatory)] [string] $RootA,
    [Parameter(Mandatory)] [string] $RootB
)

$ErrorActionPreference = 'Stop'

function Get-Binaries([string] $root) {
    $found = foreach ($tree in 'build', 'build-win32') {
        $dir = Join-Path $root $tree
        if (Test-Path $dir) {
            Get-ChildItem -LiteralPath $dir -Recurse -File -Include *.exe, *.dll |
                Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' }
        }
    }
    $prefix = (Resolve-Path $root).Path.TrimEnd('\') + '\'
    $found | ForEach-Object { $_.FullName.Substring($prefix.Length) }
}

function Get-Sha([string] $path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash }

# The first differing byte and how many differ, so the log says where
function Show-Diff([string] $a, [string] $b) {
    $x = [IO.File]::ReadAllBytes($a); $y = [IO.File]::ReadAllBytes($b)
    if ($x.Length -ne $y.Length) { Write-Host "    sizes $($x.Length) and $($y.Length)"; return }
    $first = -1; $n = 0
    for ($i = 0; $i -lt $x.Length; $i++) { if ($x[$i] -ne $y[$i]) { $n++; if ($first -lt 0) { $first = $i } } }
    Write-Host ("    {0} bytes differ, the first at offset 0x{1:X}" -f $n, $first)
}

$namesA = @(Get-Binaries $RootA)
$namesB = @(Get-Binaries $RootB)
$bad = @()
foreach ($n in ($namesA + $namesB | Sort-Object -Unique)) {
    if (($namesA -notcontains $n) -or ($namesB -notcontains $n)) { $bad += $n; Write-Host "ONLY IN ONE TREE: $n"; continue }
    $a = Join-Path $RootA $n; $b = Join-Path $RootB $n
    if ((Get-Sha $a) -ne (Get-Sha $b)) { $bad += $n; Write-Host "DIFFERS: $n"; Show-Diff $a $b }
}
Write-Host "binaries compared: $($namesA.Count); different: $($bad.Count)"

# A tree with no havne.exe, ecuio.dll or surrogate compared nothing that
# matters: an empty pair of folders would read as reproducible
$missing = @()
foreach ($want in 'havne.exe', 'ecuio.dll', 'ecuio-j2534-surrogate.exe') {
    foreach ($tree in @(@('first', $namesA), @('second', $namesB))) {
        if (-not ($tree[1] | Where-Object { (Split-Path -Leaf $_) -eq $want })) { $missing += "$want ($($tree[0]) tree)" }
    }
}
if ($missing.Count) {
    Write-Host "::error::nothing to compare for: $($missing -join ', ')"
    exit 1
}

# Reported, not gated
foreach ($z in Get-ChildItem -LiteralPath (Join-Path $RootA 'dist') -Filter *.zip -ErrorAction SilentlyContinue) {
    $other = Join-Path (Join-Path $RootB 'dist') $z.Name
    $same = (Test-Path $other) -and ((Get-Sha $z.FullName) -eq (Get-Sha $other))
    $status = if ($same) { 'match' } else { 'differ' }
    Write-Host "zip $($z.Name): $status"
    if (-not $same) { Write-Host "::warning::$($z.Name) differs between the two builds (reported, not gated)" }
}

if ($bad.Count) {
    Write-Host "::error::not reproducible: $($bad -join ', ')"
    exit 1
}
Write-Host 'reproducible: every executable and DLL matches'
