# Headless screenshots of the hub while a (fake) game runs: Controllers page + Library (qa for "live hub").
# HUB_SHOT_FAKE_RUNNING=<key|1> pretends a built-in runner plays; ext:<key> an external emulator. Muted, no window. PNGs in -Out.
param([string]$Out = "$env:TEMP\live_hub_shots", [string]$Exe = "$PSScriptRoot\..\build\bin\Release\RetroHub.exe")
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
$env:RETROHUB_NO_SHELL = "1"; $env:HUB_SHOT_PAD = "wait"; $env:HUB_SHOT_FAKE_LIB = "6"; $env:HUB_SHOT_NOWAIT = "1"; $env:HUB_SHOT_SECS = "3"; $env:RETROHUB_QA = "1"
$env:SSB64_MUTE = "1"; $env:RETRORUN_MUTE = "1"; $env:N64RUN_MUTE = "1"; $env:HUB_SHOT_SIZE = "1280x800"
function Shot($name, $vars) {
  foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
  $env:HUB_SCREENSHOT = "$Out\$name.bmp"
  $p = Start-Process -FilePath $Exe -PassThru -WindowStyle Hidden
  if (-not $p.WaitForExit(60000)) { $p.Kill() }
  Remove-Item env:HUB_SCREENSHOT
  foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
  if (Test-Path "$Out\$name.bmp") { $b = [System.Drawing.Image]::FromFile("$Out\$name.bmp"); $b.Save("$Out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose(); Remove-Item "$Out\$name.bmp" }
  Write-Output "$Out\$name.png -> $(Test-Path "$Out\$name.png")"
}
Shot "controllers_running_builtin" @{ HUB_SHOT_FAKE_RUNNING = "fake:0"; HUB_SHOT_CTL_GAME = "fake:0"; HUB_SHOT_FAKE_PADS = "2" }
Shot "controllers_running_external" @{ HUB_SHOT_FAKE_RUNNING = "ext:fake:0"; HUB_SHOT_CTL_GAME = "fake:0"; HUB_SHOT_FAKE_PADS = "2" }
Shot "library_running" @{ HUB_SHOT_FAKE_RUNNING = "fake:0" }
Shot "details_running" @{ HUB_SHOT_FAKE_RUNNING = "fake:0"; HUB_SELECT_GAME = "fake:1"; HUB_SHOT_KEYS = "i" }
