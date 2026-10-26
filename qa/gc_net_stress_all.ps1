# Fan the stress modes across parallel lanes (own wrangler port, desktop, work dir each) and wait. usage:
#   gc_net_stress_all.ps1 -Build <bin\Release> -DolphinDir <dir> -DiscDir <dir> -Tag baseline [-Runs 20] [-HoldSeconds 60]
# Results: C:\rhq\results_<Tag>_lane<k>.jsonl ; summarize with gc_net_stress_summary.ps1 -Tag <Tag>
param([Parameter(Mandatory)][string]$Build, [Parameter(Mandatory)][string]$DolphinDir, [Parameter(Mandatory)][string]$DiscDir,
      [Parameter(Mandatory)][string]$Tag, [int]$Runs = 20, [int]$HoldSeconds = 60, [int]$BasePort = 11600)
$lanes = @(@("direct","relay"), @("sim","simrelay"), @("gfirst","glate","slowhost"))
$procs = @()
for ($k = 0; $k -lt $lanes.Count; $k++) {
  $out = "C:\rhq\results_${Tag}_lane$k.jsonl"; if (Test-Path $out) { Remove-Item $out }
  $a = @("-NoProfile","-ExecutionPolicy","Bypass","-File","$PSScriptRoot\gc_net_stress.ps1","-Build",$Build,"-DolphinDir",$DolphinDir,"-DiscDir",$DiscDir,
         "-Runs","$Runs","-HoldSeconds","$HoldSeconds","-Port","$($BasePort + $k)","-Lane","$k","-Out",$out,"-Tag",$Tag,"-Modes",($lanes[$k] -join ","))
  $procs += Start-Process powershell -ArgumentList $a -WindowStyle Hidden -PassThru -RedirectStandardOutput "C:\rhq\log_${Tag}_lane$k.txt" -RedirectStandardError "C:\rhq\log_${Tag}_lane$k.err"
}
$procs | Wait-Process
