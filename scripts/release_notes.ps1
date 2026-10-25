# Prints a changelog entry as Markdown for `gh release create -F`.
#   .\scripts\release_notes.ps1 > notes.md ; gh release create vX.Y.Z -F notes.md ...
#   .\scripts\release_notes.ps1 -Version 1.21.0
# Default: the newest entry in data\changelog.json. Appends the install blurb used by every release.
param(
    [string]$Version = "",
    [string]$Json = "",
    [switch]$NoFooter
)
$ErrorActionPreference = "Stop"
if (-not $Json) { $Json = Join-Path (Split-Path $PSScriptRoot -Parent) "data\changelog.json" }
$doc = Get-Content -Raw -Encoding UTF8 $Json | ConvertFrom-Json
$entry = $null
if ($Version) {
    $want = $Version.TrimStart("vV")
    $entry = $doc.versions | Where-Object { $_.version -eq $want } | Select-Object -First 1
    if (-not $entry) { Write-Error "No entry for version $want in $Json"; exit 1 }
} else {
    $entry = $doc.versions | Select-Object -First 1
}
$sb = New-Object System.Text.StringBuilder
$title = if ($entry.title) { $entry.title } else { "RetroHub v$($entry.version)" }
[void]$sb.AppendLine("## $title")
foreach ($sec in @(@("new", "New"), @("improved", "Improved"), @("fixed", "Fixed"))) {
    $items = $entry.($sec[0])
    if (-not $items) { continue }
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**$($sec[1])**")
    foreach ($b in $items) { [void]$sb.AppendLine("- $b") }
}
if (-not $NoFooter) {
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("Both players should update before playing together.")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**Install:** new players download [RetroHub-Setup.exe](https://github.com/fatty8me2/retrohub/releases/latest/download/RetroHub-Setup.exe). Existing installs update themselves.")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("Windows SmartScreen may warn about an unrecognized app: click **More info**, then **Run anyway**.")
}
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Write-Output $sb.ToString().TrimEnd()
