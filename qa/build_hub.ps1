# Builds RetroHub (Release) at below-normal priority. Usage: qa\build_hub.ps1 [-Target RetroHub] [-CopyTo qa_out\realcopy]
param([string]$Target = "RetroHub", [string]$CopyTo = "")
$root = Split-Path $PSScriptRoot -Parent
$cm = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe | Select-Object -First 1).FullName
Set-Location $root
$p = Start-Process $cm -ArgumentList '--build', 'build', '--config', 'Release', '--target', $Target -PassThru -WindowStyle Hidden -RedirectStandardOutput "$env:TEMP\hub_build_out.txt" -RedirectStandardError "$env:TEMP\hub_build_err.txt"
$p.PriorityClass = 'BelowNormal'; $p.WaitForExit()
Select-String -Path "$env:TEMP\hub_build_out.txt" -Pattern ' error |rror C|warning C4(?!129)' | Select-Object -First 20
if ($CopyTo -and $Target -eq "RetroHub") { Copy-Item build\bin\Release\RetroHub.exe $CopyTo -Force }
"build done"
