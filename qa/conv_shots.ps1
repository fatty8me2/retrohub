# Headless screenshots for Plan 4 Wave 2 C (convenience). Muted, no window (HUB_SCREENSHOT). Output: -Out dir (default qa\shots_conv).
param([string]$Out = "$PSScriptRoot\shots_conv", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:SSB64_MUTE = "1"; $env:RETROHUB_QA = "1"
function Shot($name, $size, $vars) {
    $keys = @("HUB_SHOT_SIZE","HUB_SHOT_PALETTE","HUB_SHOT_SETTINGS_TAB","HUB_START_PAGE","HUB_SHOT_PAD","HUB_SHOT_FAKE_PADTYPE","HUB_SHOT_FAKE_BATTERY","HUB_SHOT_FAKE_PAD","HUB_SHOT_WIZARD","HUB_START_VIEW","HUB_SHOT_SECS")
    foreach ($k in $keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $env:HUB_SHOT_SIZE = $size
    foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
    $env:HUB_SCREENSHOT = "$Out\$name.bmp"
    & $Exe | Out-Null
}
foreach ($size in @("1280x800", "1600x1000")) {
    Shot "home_$size" $size @{ HUB_SHOT_PAD = "1" }
    Shot "settings_online_$size" $size @{ HUB_START_PAGE = "4"; HUB_SHOT_SETTINGS_TAB = "1" }
    Shot "settings_help_$size" $size @{ HUB_START_PAGE = "4"; HUB_SHOT_SETTINGS_TAB = "4" }
    Shot "palette_$size" $size @{ HUB_SHOT_PALETTE = "kan"; HUB_SHOT_SECS = "4"; HUB_SHOT_PAD = "0" }
    Shot "palette_empty_$size" $size @{ HUB_SHOT_PALETTE = " "; HUB_SHOT_SECS = "4"; HUB_SHOT_PAD = "0" }
}
Shot "settings_online_tall" "1280x2000" @{ HUB_START_PAGE = "4"; HUB_SHOT_SETTINGS_TAB = "1" }
Shot "settings_help_tall" "1280x1500" @{ HUB_START_PAGE = "4"; HUB_SHOT_SETTINGS_TAB = "4" }
foreach ($t in @("xbox", "ps", "switch")) {
    Shot "glyphs_$t" "1280x800" @{ HUB_SHOT_PAD = "1"; HUB_SHOT_FAKE_PADTYPE = $t; HUB_SHOT_FAKE_BATTERY = "low" }
    Shot "wizard_controller_$t" "1280x800" @{ HUB_SHOT_WIZARD = "controller"; HUB_SHOT_FAKE_PAD = "1"; HUB_SHOT_FAKE_PADTYPE = $t; HUB_SHOT_PAD = "1" }
}
Shot "wizard_online" "1280x800" @{ HUB_SHOT_WIZARD = "online" }
