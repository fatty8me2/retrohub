# Headless screenshots of the "Welcome back" card (muted, no window). Output: -Out dir (default qa\shots_welcome).
# Fake last-seen 1.31.9 so several versions collapse. final = after the animation, mid = frozen at 0.35 s, owner = with the owner-only "Show me".
param([string]$Out = "$PSScriptRoot\shots_welcome", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
$env:SSB64_MUTE = "1"; $env:RETROHUB_QA = "1"
function Shot($name, $size, $vars) {
    foreach ($k in @("HUB_SHOT_SIZE","HUB_SHOT_WELCOME","HUB_SHOT_WELCOME_FROM","HUB_SHOT_WELCOME_T","HUB_SHOT_WELCOME_OWNER","HUB_SHOT_SECS","HUB_SHOT_PAD")) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $env:HUB_SHOT_SIZE = $size; $env:HUB_SHOT_WELCOME = "1"; $env:HUB_SHOT_WELCOME_FROM = "1.31.9"
    foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
    $env:HUB_SCREENSHOT = "$Out\$name.bmp"
    & $Exe | Out-Null
}
foreach ($size in @("1280x800", "1600x1000")) {
    Shot "welcome_final_$size" $size @{ HUB_SHOT_SECS = "6" }
    Shot "welcome_mid_$size" $size @{ HUB_SHOT_SECS = "6"; HUB_SHOT_WELCOME_T = "0.35" }
    Shot "welcome_owner_$size" $size @{ HUB_SHOT_SECS = "6"; HUB_SHOT_WELCOME_OWNER = "1" }
}
