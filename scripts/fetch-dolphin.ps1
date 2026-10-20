# Downloads the official Dolphin Windows x64 stable build (2509) from
# dolphin-emu.org, extracts it into Hub\deps\dolphin\ with the built-in
# Windows tar (bsdtar reads .7z), and creates an empty portable.txt next to
# Dolphin.exe so Dolphin keeps all its user data inside that folder.
# Windows PowerShell 5.1 compatible. An existing download is reused, and an
# existing extraction (Dolphin.exe present) is not extracted again.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$hubRoot = Split-Path -Parent $PSScriptRoot
$depsDir = Join-Path $hubRoot 'deps\dolphin'
$dlDir   = Join-Path $depsDir '_downloads'

$url     = 'https://dl.dolphin-emu.org/releases/2509/dolphin-2509-x64.7z'
$archive = Join-Path $dlDir 'dolphin-2509-x64.7z'
$tarExe  = Join-Path $env:SystemRoot 'System32\tar.exe'

New-Item -ItemType Directory -Force -Path $depsDir, $dlDir | Out-Null

if (Test-Path -LiteralPath $archive) {
    Write-Host "Already downloaded: $(Split-Path -Leaf $archive)"
} else {
    Write-Host "Downloading $url"
    $part = $archive + '.part'
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $part
    Move-Item -LiteralPath $part -Destination $archive -Force
}

$exe = Join-Path $depsDir 'Dolphin.exe'
if (Test-Path -LiteralPath $exe) {
    Write-Host "Already extracted: $exe"
} else {
    if (-not (Test-Path -LiteralPath $tarExe)) { throw "Windows tar not found at $tarExe" }

    # The 7z wraps everything in a Dolphin-x64 folder, so extract to a staging
    # folder, find Dolphin.exe, and move its folder's contents up into deps\dolphin.
    $stage = Join-Path $depsDir '_extract'
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage | Out-Null

    Write-Host "Extracting $(Split-Path -Leaf $archive) with $tarExe"
    & $tarExe -xf $archive -C $stage
    if ($LASTEXITCODE -ne 0) { throw "tar exited with code $LASTEXITCODE" }

    $hit = Get-ChildItem -LiteralPath $stage -Recurse -File -Filter 'Dolphin.exe' | Select-Object -First 1
    if (-not $hit) { throw "Dolphin.exe not found in the archive" }

    Get-ChildItem -LiteralPath $hit.DirectoryName |
        Move-Item -Destination $depsDir -Force
    Remove-Item -LiteralPath $stage -Recurse -Force
}

# Portable mode: Dolphin uses the folder beside Dolphin.exe for User/ and Config/.
$portable = Join-Path $depsDir 'portable.txt'
if (-not (Test-Path -LiteralPath $portable)) {
    New-Item -ItemType File -Path $portable | Out-Null
    Write-Host "Created $portable"
} else {
    Write-Host "portable.txt already present"
}

Write-Host ''
Write-Host "Top level of ${depsDir}:"
Get-ChildItem -LiteralPath $depsDir |
    Sort-Object Name |
    Format-Table -Property Name, Length, LastWriteTime -AutoSize |
    Out-String -Width 200 |
    Write-Host

$total = (Get-ChildItem -LiteralPath $depsDir -Recurse -File |
    Where-Object { $_.FullName -notlike '*\_downloads\*' } |
    Measure-Object -Property Length -Sum).Sum
Write-Host ("Installed size (excluding _downloads): {0:N1} MB" -f ($total / 1MB))
