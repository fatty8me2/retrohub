# Detached launcher: starts gc_net_stress_all.ps1 hidden and prints its PID. usage: gc_net_stress_launch.ps1 -Build <dir> -Tag <name> [-Runs 20]
param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$Tag, [int]$Runs = 20, [int]$HoldSeconds = 60,
      [string]$DiscDir = "C:\rhq\mkdd_disc", [string]$DolphinDir = "")
if (-not $DolphinDir) { $DolphinDir = (Resolve-Path "$PSScriptRoot\..\deps\dolphin").Path }
$p = Start-Process powershell -ArgumentList @("-NoProfile","-ExecutionPolicy","Bypass","-File","$PSScriptRoot\gc_net_stress_all.ps1","-Build",$Build,"-DolphinDir",$DolphinDir,"-DiscDir",$DiscDir,"-Tag",$Tag,"-Runs","$Runs","-HoldSeconds","$HoldSeconds") -WindowStyle Hidden -PassThru
$p.Id
