# Cover-art coverage check (read-only, headless, no windows). Loads the REAL shared-pack index (community list from GitHub,
# Drive folder listings), runs the hub's art resolution for every pack ghost and every library title, then fetches the
# thumbnail image (never any game file). One TSV line per game -> qa\out\pack_art.tsv, summary on the console.
# Exit code 1 when anything is missing. Usage: qa\pack_art_check.ps1 [-Library <library_cache.json>] [-Extra <links>]
param(
    [string]$Exe = "$PSScriptRoot\..\build\bin\Release\qa_pack_art.exe",
    [string]$Library = "C:\Users\fatty\Desktop\smashClassic\userdata\library_cache.json",
    [string]$Extra = ""
)
$out = "$PSScriptRoot\out"; New-Item -ItemType Directory -Force $out | Out-Null
$cache = "$out\pack_art_cache"
Remove-Item "$cache\misses.json", "$cache\names.json" -ErrorAction SilentlyContinue   # re-resolve everything; DATs / images stay cached
$a = @($cache)
if ($Library -and (Test-Path $Library)) { $a += @("--library", $Library) }
if ($Extra) { $a += @("--extra", $Extra) }
$err = & $Exe @a 2>"$out\pack_art.err" | Tee-Object "$out\pack_art.tsv"
Get-Content "$out\pack_art.err"
$miss = Get-Content "$out\pack_art.tsv" | Where-Object { ($_ -split "`t")[2] -ne "OK" }
if ($miss) { "`nMISSING:"; $miss | ForEach-Object { $f = $_ -split "`t"; "{0} {1} [{2}] {3} | {4} | {5}" -f $f[0], $f[1], $f[2], $f[5], $f[6], $f[4] }; exit 1 }
"`nall resolved"; exit 0
