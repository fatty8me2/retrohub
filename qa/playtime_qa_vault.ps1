# Playtime QA, lane "vault": the game page must read the REAL play logs from a scratch vault (no made-up data).
# Does NOT set HUB_SHOT_PLAYTIME or HUB_SHOT_FAKE_AZ. Headless (SDL dummy driver), muted, fake library of 8 games.
# Never touches the owner's install or the real vault: HUB_SHOT_VAULT points at <Out>\vault, which this script wipes first.
# usage: qa\playtime_qa_vault.ps1 [-Out <dir>] [-Exe <RetroHub.exe>]
# Through the gate (the gate passes no arguments, so the default -Out is the lane's scratch dir):
#   powershell -NoProfile -ExecutionPolicy Bypass -File <gate.ps1> -Lane vault -Script <this file> -Log <dir>\gate.log
# Output: <Out>\<name>.png per shot, <Out>\sheet_vault.png, <Out>\tree_before.txt, <Out>\tree_after.txt, <Out>\*.trace.txt
# Exit code: 0 when every shot was produced and the vault tree was checked, 1 otherwise.
param([string]$Out = 'C:\Users\fatty\AppData\Local\Temp\claude\a82a6632bfc5ba1b\c9e79f3c-c9ea-4754-b3cd-65f95c7454b4\scratchpad\playtime\qa_vault',
      [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing

$Vault = Join-Path $Out 'vault'
if (-not ($Vault -like '*qa_vault*')) { "REFUSED: vault path is not a QA scratch dir: $Vault"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem -LiteralPath $Out -Filter '*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -LiteralPath $Out -Filter '*.trace.txt' -ErrorAction SilentlyContinue | Remove-Item -Force
if (Test-Path -LiteralPath $Vault) { Remove-Item -LiteralPath $Vault -Recurse -Force }
New-Item -ItemType Directory -Force $Vault | Out-Null

$env:RETROHUB_NO_SHELL = '1'; $env:HUB_SHOT_NOWAIT = '1'; $env:HUB_SHOT_SECS = '3'; $env:RETROHUB_QA = '1'
$env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
# Nothing inherited from the shell may add made-up play data or pick a different vault.
foreach ($k in 'HUB_SHOT_PLAYTIME', 'HUB_SHOT_FAKE_AZ', 'HUB_SELECT_GAME', 'HUB_SHOT_KEYS', 'HUB_SHOT_SIZE', 'HUB_SHOT_FAKE_LIB', 'HUB_SHOT_VAULT', 'HUB_SHOT_SWITCH', 'HUB_START_VIEW', 'HUB_SHOT_SORT', 'HUB_SHOT_LIBVIEW') {
    Remove-Item "env:$k" -ErrorAction SilentlyContinue
}
$script:made = @()

function Shot([string]$name, [hashtable]$vars) {
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $bmp = Join-Path $Out "$name.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $env:HUB_SHOT_TRACE = Join-Path $Out "$name.trace.txt"
    Remove-Item -LiteralPath $bmp -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }   # our own child, by PID
    Remove-Item env:HUB_SCREENSHOT, env:HUB_SHOT_TRACE -ErrorAction SilentlyContinue
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $bmp) {
        $png = Join-Path $Out "$name.png"
        $img = [System.Drawing.Image]::FromFile($bmp)
        $img.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
        Remove-Item -LiteralPath $bmp
        $script:made += $png
        "SHOT OK   $name"
    } else { "SHOT MISSING  $name" }
}

# ---- fixtures: real playlog files in the scratch vault (<vault>\<system>\<key with ':' as '_'>\playlog\<PC>.log) ----
$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$PC = ($env:COMPUTERNAME -replace '[^A-Za-z0-9-]', '_')
function Write-Playlog([string]$sys, [string]$key, [string]$fileName, [string[]]$lines) {
    $dir = Join-Path (Join-Path (Join-Path $Vault $sys) ($key -replace ':', '_')) 'playlog'
    New-Item -ItemType Directory -Force $dir | Out-Null
    $text = (($lines -join "`n") + "`n")
    [IO.File]::WriteAllText((Join-Path $dir $fileName), $text, [Text.UTF8Encoding]::new($false))
}

# A. N64 fake:0, this PC (with junk lines) + LAPTOP + a .bak that must be ignored.
Write-Playlog 'n64' 'fake:0' "$PC.log" @(
    "$($now - 2*86400),3600", "$($now - 86400),5400", "$($now - 3*3600),1800", 'garbage', '123', ',', '')
Write-Playlog 'n64' 'fake:0' 'LAPTOP.log' @("$($now - 20*86400),7200")
Write-Playlog 'n64' 'fake:0' 'LAPTOP.log.bak' @("$($now - 5*86400),360000")
# B. GBA fake:1, only other PCs.
Write-Playlog 'gba' 'fake:1' 'LAPTOP.log' @("$($now - 30*86400),3600")
Write-Playlog 'gba' 'fake:1' 'DESK2.log' @("$($now - 40*86400),1800")
# C. GameCube fake:2, this PC in lower case.
Write-Playlog 'gc' 'fake:2' ("$PC".ToLower() + '.log') @("$($now - 86400),1440")
# D. fake:3 has no folder at all.

"FIXTURES written: PC=$PC now=$now"
$Tree = { Get-ChildItem -LiteralPath $Vault -Recurse -Force | ForEach-Object {
    $_.FullName.Substring($Vault.Length).TrimStart('\') + $(if ($_.PSIsContainer) { '\' } else { '' }) } | Sort-Object }
$before = @(& $Tree)
$before | Set-Content -LiteralPath (Join-Path $Out 'tree_before.txt') -Encoding utf8

# ---- shots: the game page of each fixture game, 1600x1000, vault from HUB_SHOT_VAULT ----
$page = @{ HUB_SHOT_SIZE = '1600x1000'; HUB_SHOT_FAKE_LIB = '8'; HUB_SHOT_VAULT = $Vault; HUB_SHOT_KEYS = 'i' }
Shot 'vault_A_n64_fake0' ($page + @{ HUB_SELECT_GAME = 'fake:0' })
Shot 'vault_B_gba_fake1' ($page + @{ HUB_SELECT_GAME = 'fake:1' })
Shot 'vault_C_gc_fake2' ($page + @{ HUB_SELECT_GAME = 'fake:2' })
Shot 'vault_D_n64_fake3_nofolder' ($page + @{ HUB_SELECT_GAME = 'fake:3' })

# ---- vault tree: opening pages must create nothing ----
$after = @(& $Tree)
$after | Set-Content -LiteralPath (Join-Path $Out 'tree_after.txt') -Encoding utf8
$created = @($after | Where-Object { $before -notcontains $_ })
$removed = @($before | Where-Object { $after -notcontains $_ })
"TREE created: $(if ($created) { $created -join ' | ' } else { '(none)' })"
"TREE removed: $(if ($removed) { $removed -join ' | ' } else { '(none)' })"
$fake3 = Test-Path -LiteralPath (Join-Path $Vault 'n64\fake_3')
"CHECK D: n64\fake_3 exists after the run = $fake3 (must be False)"

$sheet = Join-Path $PSScriptRoot 'contact_sheet.ps1'
if ($script:made.Count -and (Test-Path -LiteralPath $sheet)) { & $sheet (Join-Path $Out 'sheet_vault.png') @script:made -Cols 2 -Full }
"SHOTS $($script:made.Count) of 4"

$ok = ($script:made.Count -eq 4) -and (-not $fake3)
if ($ok) { exit 0 } else { exit 1 }
