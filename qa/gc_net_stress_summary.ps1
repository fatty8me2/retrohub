# Summarize results_<Tag>_lane*.jsonl into a markdown table (success rate per mode, failure reasons, start-time stats).
param([Parameter(Mandatory)][string]$Tag, [string]$Dir = "C:\rhq")
$rows = Get-ChildItem $Dir -Filter "results_${Tag}_lane*.jsonl" | ForEach-Object { Get-Content $_.FullName -Encoding utf8 | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json } }
$harness = @($rows | Where-Object { $_.reason -like 'harness exception*' }).Count
$rows = @($rows | Where-Object { $_.reason -notlike 'harness exception*' })   # harness-side cleanup errors are not product results
"(excluded $harness runs lost to harness exceptions)"
$order = "direct","relay","sim","simrelay","gfirst","glate","slowhost"
"| mode | runs | pass | started | held 60s | desync | median start s | max start s |"
"|---|---|---|---|---|---|---|---|"
foreach ($m in $order) {
  $r = @($rows | Where-Object { $_.mode -eq $m }); if (-not $r.Count) { continue }
  $st = @($r | Where-Object { $_.started } | ForEach-Object { [double]$_.start_s } | Sort-Object)
  $med = if ($st.Count) { $st[[int][math]::Floor(($st.Count - 1) / 2)] } else { "-" }
  $mx = if ($st.Count) { $st[-1] } else { "-" }
  "| $m | $($r.Count) | $(@($r | Where-Object pass).Count) | $(@($r | Where-Object started).Count) | $(@($r | Where-Object held).Count) | $(@($r | Where-Object desync).Count) | $med | $mx |"
}
""
"Failure reasons:"
$rows | Where-Object { -not $_.pass } | Group-Object mode, reason | ForEach-Object { $d = ([string]$_.Group[0].detail -replace '\s+', ' '); "- {0}: {1} x  e.g. {2}" -f $_.Name, $_.Count, $d.Substring(0, [Math]::Min(260, $d.Length)) }
