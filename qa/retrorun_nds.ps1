# Headless Nintendo DS QA for retrorun (no window, muted). Never writes next to the ROM: saves/states go to a scratch dir.
# Usage: powershell -ExecutionPolicy Bypass -File qa\retrorun_nds.ps1 -Rom <game.nds> [-Frames 1500] [-Out <scratch dir>]
# Checks: boots + screenshot (256x384 vertical stack), seeded .srm is loaded into the core's save RAM, stylus input reaches the core (cursor pixels differ), no auto.state is written for DS.
param(
    [Parameter(Mandatory = $true)][string]$Rom,
    [int]$Frames = 1500,
    [string]$Out = (Join-Path $env:TEMP 'retrorun_nds_qa')
)
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\retrorun.exe'
$core = Join-Path $root 'deps\cores\melondsds_libretro.dll'
if (-not (Test-Path $exe)) { throw "build retrorun first: $exe" }
if (-not (Test-Path $core)) { throw "missing core (run scripts\fetch-cores.ps1): $core" }
Remove-Item -Recurse -Force $Out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$Out\sv", "$Out\st" | Out-Null
$stem = [IO.Path]::GetFileNameWithoutExtension($Rom)
$fail = 0
function Check($ok, $what) { if ($ok) { Write-Host "PASS $what" } else { Write-Host "FAIL $what"; $script:fail++ } }

function Run($extra, $shot) {
    $a = @('--core', $core, '--rom', $Rom, '--save-dir', "$Out\sv", '--state-dir', "$Out\st", '--system', 'nds', '--headless',
           '--frames', $Frames, '--screenshot', $shot) + $extra
    & $exe @a 2>&1 | Out-String
}

# 1. boot + screenshot
$log = Run @() "$Out\boot.png"
Check ($log -match 'av: 256x384') 'default layout is a vertical stack (256x384)'
Check ($log -notmatch 'WARNING: DS core option') 'every option retrorun sets exists in the real core with that value'
Check ($log -match 'battery RAM \d+ bytes') 'core exposes battery RAM'
Check (Test-Path "$Out\boot.png") 'screenshot written'
Check (-not (Test-Path "$Out\st\auto.state")) 'no auto.state for DS'
Check (@(Get-ChildItem "$Out\sv" -Recurse -File).Count -eq 0) 'no stray files (SD card images, firmware) in the save dir'

# 2. save wiring: a seeded <stem>.srm is loaded into the core's save RAM
$marker = [Text.Encoding]::ASCII.GetBytes('RETROHUBMARKER-0123456789abcdef-XYZ')
$srm = New-Object byte[] 524288
for ($i = 0; $i -lt $srm.Length; $i++) { $srm[$i] = 0xFF }
[Array]::Copy($marker, 0, $srm, 0, $marker.Length)
[IO.File]::WriteAllBytes("$Out\sv\$stem.srm", $srm)
$log = Run @() "$Out\seed.png"
Check ($log -match "save: loaded .*$([regex]::Escape($stem))\.srm \(524288 bytes") 'seeded .srm loaded from the battery dir'
# Note: the DS core's in-memory unserialize is not frame-exact (--selftest reports DIFFERENT), one reason online play is off for DS.

# 3. stylus reaches the core: with the cursor always drawn, holding the stylus changes the bottom screen
Run @('--opt', 'melonds_show_cursor=always') "$Out\nostylus.png" | Out-Null
Run @('--opt', 'melonds_show_cursor=always', '--stylus', '0.5,0.75') "$Out\stylus.png" | Out-Null
$h1 = (Get-FileHash "$Out\nostylus.png").Hash
$h2 = (Get-FileHash "$Out\stylus.png").Hash
Check ($h1 -ne $h2) 'stylus input changes the frame (cursor drawn on the touch screen)'

Write-Host "screenshots in $Out"
if ($fail) { Write-Host "$fail check(s) FAILED"; exit 1 }
Write-Host 'retrorun_nds: all checks passed'
