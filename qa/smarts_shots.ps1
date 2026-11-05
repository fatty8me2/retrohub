# Headless screenshots of Plan 4 L1 smart shelves + L2 completion/notes. Scratch vault only, muted, no visible window.
# Usage: powershell -NoProfile -File qa\smarts_shots.ps1 [outdir] [WxH]
param([string]$Out = "$env:TEMP\smarts_shots", [string]$Size = '1280x800')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$exe = Join-Path $root 'build\bin\Release\RetroHub.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$vault = Join-Path $Out 'vault'
Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
$prefs = '{"user_shelves":[{"name":"Co-op night","keys":["fake:0","fake:3"]},{"name":"N64 backlog","keys":[],"rule":{"systems":["n64"],"genre":"","min_players":2,"status":"","favorites_only":false,"not_played_days":0,"decade":0}}],' +
         '"progress":{"fake:0":{"s":"beaten","n":"Finished on hard. Try the bonus stages next time."},"fake:3":{"s":"backlog","n":""},"fake:1":{"s":"playing","n":""}},' +
         '"ts":{"user_shelves":1000,"progress":1000}}'
[IO.File]::WriteAllText((Join-Path $vault '_account\prefs.json'), $prefs)
$env:HUB_SHOT_VAULT = $vault
$env:HUB_SHOT_FAKE_LIB = '12'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
$env:HUB_SHOT_SECS = '3'
$env:HUB_SHOT_SIZE = $Size
# name, HUB_SHOT_CUSTOM, HUB_SHOT_DETAILS
$shots = @(@('home', '', ''), @('details', '', 'fake:0'), @('smart_new', 'smart', ''), @('smart_edit', 'smart_edit', ''), @('manager', 'manager', ''), @('home_smart', 'home_smart', ''))
foreach ($s in $shots) {
    foreach ($v in 'HUB_SHOT_CUSTOM', 'HUB_SHOT_DETAILS') { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
    if ($s[1]) { $env:HUB_SHOT_CUSTOM = $s[1] }
    Remove-Item Env:HUB_SHOT_WHEEL -ErrorAction SilentlyContinue; $env:HUB_SHOT_SECS = '3'
    if ($s[2]) { $env:HUB_SHOT_DETAILS = $s[2] }
    $env:HUB_SCREENSHOT = Join-Path $Out "$($s[0])-$Size.bmp"
    & $exe | Out-Null
    Write-Host "$($s[0]) -> $($env:HUB_SCREENSHOT) $(Test-Path $env:HUB_SCREENSHOT)"
}
