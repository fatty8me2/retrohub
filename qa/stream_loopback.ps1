# Remote play loopback: streamrun --host (test pattern source) + streamrun --guest (headless) through a LOCAL worker.
# Run via the local-worker helper so RETROHUB_TEST_SERVER is set and the worker is stopped afterwards:
#   node qa\online_local.mjs 12000 powershell -NoProfile -File qa\stream_loopback.ps1 -Build build\bin\Release [-Seconds 15] [-Extra "--sw-encode"]
# Both processes are console children with no window (CreateNoWindow); nothing is shown on the user's screen.
param(
  [Parameter(Mandatory)][string]$Build, [int]$Seconds = 15, [string]$HostArgs = "", [string]$GuestArgs = "",
  [string]$Capture = "", [string]$Out = ""
)
. "$PSScriptRoot\_guard.ps1"
$Server = $env:RETROHUB_TEST_SERVER
if (-not $Server) { Write-Error "RETROHUB_TEST_SERVER not set (use qa\online_local.mjs)"; exit 2 }
Assert-LocalServer $Server
$exe = Join-Path (Resolve-Path $Build) "streamrun.exe"
if (-not (Test-Path $exe)) { Write-Error "missing $exe"; exit 2 }
if (-not $Out) { $Out = Join-Path $env:TEMP "streamrun_loopback" }
New-Item -ItemType Directory -Force $Out | Out-Null
$alpha = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'.ToCharArray()
$room = (-join (1..6 | ForEach-Object { $alpha | Get-Random })) + "-1"
$env:SSB64_MUTE = "1"; $env:HUBNET_FORCE_RELAY = "0"

function Start-Child($args_, $name) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe; $psi.Arguments = $args_; $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
  $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
  $p = New-Object System.Diagnostics.Process; $p.StartInfo = $psi
  $log = Join-Path $Out "$name.log"; Remove-Item $log -ErrorAction SilentlyContinue
  $null = Register-ObjectEvent $p ErrorDataReceived -Action { if ($EventArgs.Data) { Add-Content -Path $Event.MessageData -Value $EventArgs.Data } } -MessageData $log
  $null = $p.Start(); $p.BeginErrorReadLine(); $p.BeginOutputReadLine()
  return $p
}
$hostStats = Join-Path $Out "host.json"; $guestStats = Join-Path $Out "guest.json"
Remove-Item $hostStats, $guestStats -ErrorAction SilentlyContinue
$src = if ($Capture) { "--capture-hwnd $Capture" } else { "--test-pattern" }
$h = Start-Child "--host $src --net $Server --room $room --slot 0 --duration $($Seconds + 4) --stats-file `"$hostStats`" --pad-port 26999 $HostArgs" "host"
Start-Sleep -Milliseconds 800
$g = Start-Child "--guest --headless --test-input --net $Server --room $room --slot 1 --duration $Seconds --stats-file `"$guestStats`" $GuestArgs" "guest"
$null = $g.WaitForExit(($Seconds + 40) * 1000)
$null = $h.WaitForExit(20000)
foreach ($p in @($g, $h)) { if (-not $p.HasExited) { $p.Kill() } }
Start-Sleep -Milliseconds 300
"--- host.log"; Get-Content (Join-Path $Out "host.log") -ErrorAction SilentlyContinue
"--- guest.log"; Get-Content (Join-Path $Out "guest.log") -ErrorAction SilentlyContinue
"--- host.json"; Get-Content $hostStats -ErrorAction SilentlyContinue
"--- guest.json"; Get-Content $guestStats -ErrorAction SilentlyContinue
exit ([int]($g.ExitCode))
