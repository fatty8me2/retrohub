# Downloads the official mupen64plus 2.6.0 Windows bundle and the GLideN64
# plugin, and stages the files the Classic Hub runtime needs into deps\emu\.
# Windows PowerShell 5.1 compatible. Existing zips are reused, not re-downloaded.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$hubRoot = Split-Path -Parent $PSScriptRoot
$depsDir = Join-Path $hubRoot 'deps\emu'
$dlDir   = Join-Path $depsDir '_downloads'
$m64Dir  = Join-Path $depsDir '_m64p'
$glDir   = Join-Path $depsDir '_gliden64'

$bundleUrl = 'https://github.com/mupen64plus/mupen64plus-core/releases/download/2.6.0/mupen64plus-bundle-win64-2.6.0.zip'
$glUrl     = 'https://github.com/gonetz/GLideN64/releases/download/github-actions/GLideN64-a04081a-Windows-Mupen64Plus-CLI-x64.zip'
$bundleZip = Join-Path $dlDir 'mupen64plus-bundle-win64-2.6.0.zip'
$glZip     = Join-Path $dlDir 'GLideN64-a04081a-Windows-Mupen64Plus-CLI-x64.zip'

New-Item -ItemType Directory -Force -Path $depsDir, $dlDir | Out-Null

function Get-IfMissing([string]$url, [string]$path) {
    if (Test-Path -LiteralPath $path) {
        Write-Host "Already downloaded: $(Split-Path -Leaf $path)"
        return
    }
    Write-Host "Downloading $url"
    $part = $path + '.part'
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $part
    Move-Item -LiteralPath $part -Destination $path -Force
}

function Expand-Fresh([string]$zip, [string]$dest) {
    if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
}

# Returns the directory that contains a file of the given name, searching recursively.
function Find-DirOf([string]$root, [string]$name) {
    $hit = Get-ChildItem -LiteralPath $root -Recurse -File -Filter $name | Select-Object -First 1
    if (-not $hit) { throw "Missing $name under $root" }
    return $hit.DirectoryName
}

function Copy-Staged([string]$srcDir, [string[]]$names, [bool]$required) {
    foreach ($n in $names) {
        $src = Join-Path $srcDir $n
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $depsDir $n) -Force
        } elseif ($required) {
            throw "Required file $n not found in $srcDir"
        } else {
            Write-Host "Optional file not present, skipped: $n"
        }
    }
}

Get-IfMissing $bundleUrl $bundleZip
Get-IfMissing $glUrl $glZip

Expand-Fresh $bundleZip $m64Dir
Expand-Fresh $glZip $glDir

# mupen64plus core, RSP-HLE, audio-SDL, their runtime DLLs, and config/font data.
$m64Src = Find-DirOf $m64Dir 'mupen64plus.dll'
Copy-Staged $m64Src @(
    'mupen64plus.dll',
    'mupen64plus-rsp-hle.dll',
    'mupen64plus-audio-sdl.dll',
    'SDL2.dll',
    'SDL2_net.dll',
    'freetype.dll',
    'libpng16.dll',
    'zlib.dll',
    'mupen64plus.ini',
    'InputAutoCfg.ini',
    'font.ttf'
) $true

# GLideN64 video plugin and its config.
$glSrc = Find-DirOf $glDir 'mupen64plus-video-GLideN64.dll'
Copy-Staged $glSrc @(
    'mupen64plus-video-GLideN64.dll',
    'GLideN64.custom.ini'
) $true

Write-Host ''
Write-Host "Staged files in ${depsDir}:"
Get-ChildItem -LiteralPath $depsDir -File |
    Sort-Object Name |
    Format-Table -Property Name, Length -AutoSize |
    Out-String -Width 200 |
    Write-Host
