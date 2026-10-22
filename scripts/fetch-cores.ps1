# Downloads the official libretro Windows x64 cores from the libretro nightly
# buildbot, plus the libretro frontend info files for them, and stages them into
# deps\cores\ (DLLs) and deps\cores\info\ (.info files).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\fetch-cores.ps1 [-Force]
# Existing DLLs and .info files are skipped unless -Force is given.
# Windows PowerShell 5.1 compatible.

param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$hubRoot    = Split-Path -Parent $PSScriptRoot
$coresDir   = Join-Path $hubRoot 'deps\cores'
$infoDir    = Join-Path $coresDir 'info'
$dlDir      = Join-Path $coresDir '_downloads'

$coreBase   = 'https://buildbot.libretro.com/nightly/windows/x86_64/latest/'
$infoZipUrl = 'https://buildbot.libretro.com/assets/frontend/info.zip'
$infoZip    = Join-Path $dlDir 'info.zip'

$cores = @(
    'mgba_libretro',       # GBA (also GB/GBC)
    'gambatte_libretro',   # GB/GBC
    'sameboy_libretro',    # GB/GBC, accuracy-focused
    'melondsds_libretro'   # Nintendo DS (melonDS DS; software renderer, built-in FreeBIOS/firmware)
)

New-Item -ItemType Directory -Force -Path $coresDir, $dlDir, $infoDir | Out-Null

function Get-Download([string]$url, [string]$path) {
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        Write-Host "Already downloaded: $(Split-Path -Leaf $path)"
        return
    }
    Write-Host "Downloading $url"
    $part = $path + '.part'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $part
    Move-Item -LiteralPath $part -Destination $path -Force
}

# Extracts a zip into a fresh directory and returns the path of the named file.
function Expand-Find([string]$zip, [string]$dest, [string]$name) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
    $hit = Get-ChildItem -LiteralPath $dest -Recurse -File -Filter $name | Select-Object -First 1
    if (-not $hit) { throw "Missing $name in $zip" }
    return $hit.FullName
}

# 1. Core DLLs.
foreach ($core in $cores) {
    $dllName = $core + '.dll'
    $dllPath = Join-Path $coresDir $dllName
    if ((Test-Path -LiteralPath $dllPath) -and -not $Force) {
        Write-Host "Skip (exists): $dllName"
        continue
    }
    $zip = Join-Path $dlDir ($dllName + '.zip')
    Get-Download ($coreBase + $dllName + '.zip') $zip
    $extDir = Join-Path $dlDir ('_x_' + $core)
    $src = Expand-Find $zip $extDir $dllName
    Copy-Item -LiteralPath $src -Destination $dllPath -Force
    Remove-Item -LiteralPath $extDir -Recurse -Force
    Write-Host "Installed: $dllName"
}

# 2. Info files (only the ones for the cores above).
$infoNeeded = @()
foreach ($core in $cores) { $infoNeeded += ($core + '.info') }

$missingInfo = @()
foreach ($n in $infoNeeded) {
    if (-not (Test-Path -LiteralPath (Join-Path $infoDir $n)) -or $Force) { $missingInfo += $n }
}

if ($missingInfo.Count -gt 0) {
    Get-Download $infoZipUrl $infoZip
    $extInfo = Join-Path $dlDir '_x_info'
    if (Test-Path -LiteralPath $extInfo) { Remove-Item -LiteralPath $extInfo -Recurse -Force }
    Expand-Archive -LiteralPath $infoZip -DestinationPath $extInfo -Force
    foreach ($n in $missingInfo) {
        $hit = Get-ChildItem -LiteralPath $extInfo -Recurse -File -Filter $n | Select-Object -First 1
        if (-not $hit) { throw "Missing $n in info.zip" }
        Copy-Item -LiteralPath $hit.FullName -Destination (Join-Path $infoDir $n) -Force
        Write-Host "Installed: info\$n"
    }
    Remove-Item -LiteralPath $extInfo -Recurse -Force
} else {
    Write-Host "Skip (exists): info files"
}

# 3. Summary table: core, DLL size, display_version from the .info file.
$rows = @()
foreach ($core in $cores) {
    $dllPath  = Join-Path $coresDir ($core + '.dll')
    $infoPath = Join-Path $infoDir ($core + '.info')
    $sizeKB = 'missing'
    if (Test-Path -LiteralPath $dllPath) {
        $sizeKB = [string][math]::Round((Get-Item -LiteralPath $dllPath).Length / 1KB) + ' KB'
    }
    $ver = 'unknown'
    if (Test-Path -LiteralPath $infoPath) {
        foreach ($line in Get-Content -LiteralPath $infoPath) {
            if ($line -match '^\s*display_version\s*=\s*"(.*)"\s*$') { $ver = $Matches[1]; break }
        }
    }
    $rows += New-Object PSObject -Property @{
        Core    = $core
        Size    = $sizeKB
        Version = $ver
    }
}

Write-Host ''
$rows | Format-Table -Property Core, Size, Version -AutoSize | Out-String | Write-Host
