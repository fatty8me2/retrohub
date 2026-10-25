# Headless screenshots of the cloud saves v2 UI (Saves page states, dialogs, wizard step). No window is shown, no network.
# usage: qa\cloud_ui_shots.ps1 -Out <dir> [-Exe <RetroHub.exe>]   (writes <name>_1280x800.png and <name>_1600x1000.png)
param([string]$Out = "$env:TEMP\cloud_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"
$env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:SSB64_MUTE = "1"
function Shot($name, $vars) {
  foreach ($size in @("1280x800", "1600x1000")) {
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    if (-not $vars.ContainsKey("HUB_SHOT_SIZE")) { $env:HUB_SHOT_SIZE = $size }
    $bmp = "$Out\${name}_$size.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(60000)) { $p.Kill() }
    Remove-Item env:HUB_SCREENSHOT; Remove-Item env:HUB_SHOT_SIZE -ErrorAction SilentlyContinue
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path $bmp) { $im = [System.Drawing.Image]::FromFile($bmp); $im.Save("$Out\${name}_$size.png", [System.Drawing.Imaging.ImageFormat]::Png); $im.Dispose(); Remove-Item $bmp }
    Write-Output "${name}_$size -> $(Test-Path "$Out\${name}_$size.png")"
  }
}
$saves = @{ HUB_SHOT_PAGE = "saves" }
Shot "saves_ok"        ($saves + @{ HUB_SHOT_CLOUD2 = "ok" })
Shot "saves_mixed"     ($saves + @{ HUB_SHOT_CLOUD2 = "mixed" })
Shot "saves_history"   ($saves + @{ HUB_SHOT_CLOUD2 = "mixed"; HUB_SHOT_CLOUD_OPEN = "gba/pokemon-emerald" })
Shot "saves_offline"   ($saves + @{ HUB_SHOT_CLOUD2 = "offline" })
Shot "saves_over"      ($saves + @{ HUB_SHOT_CLOUD2 = "over" })
Shot "saves_attention" ($saves + @{ HUB_SHOT_CLOUD2 = "attention" })
Shot "saves_paused"    ($saves + @{ HUB_SHOT_CLOUD2 = "paused" })
Shot "dlg_conflict"    ($saves + @{ HUB_SHOT_CLOUD2 = "mixed"; HUB_SHOT_CLOUD_DIALOG = "conflict" })
Shot "dlg_lease"       ($saves + @{ HUB_SHOT_CLOUD2 = "mixed"; HUB_SHOT_CLOUD_DIALOG = "lease" })
Shot "launch_checking" @{ HUB_SHOT_CLOUD2 = "ok"; HUB_SHOT_CLOUD_DIALOG = "checking" }
Shot "launch_notices"  @{ HUB_SHOT_CLOUD2 = "ok"; HUB_SHOT_CLOUD_DIALOG = "pulled" }
Shot "wizard_cloud"    @{ HUB_SHOT_CLOUD2 = "ok"; HUB_SHOT_WIZARD = "cloud" }
Shot "wizard_cloud_pad" @{ HUB_SHOT_CLOUD2 = "ok"; HUB_SHOT_WIZARD = "cloud"; HUB_SHOT_PAD = "1" }
Shot "library_tiles"   @{ HUB_SHOT_CLOUD2 = "mixed" }
# Tall captures so every row of the games list is visible (the 1280x800 shots stop at the header card).
Shot "tall_mixed"   ($saves + @{ HUB_SHOT_CLOUD2 = "mixed"; HUB_SHOT_SIZE = "1280x1900" })
Shot "tall_history" ($saves + @{ HUB_SHOT_CLOUD2 = "mixed"; HUB_SHOT_CLOUD_OPEN = "gba/pokemon-emerald"; HUB_SHOT_SIZE = "1280x1900" })
