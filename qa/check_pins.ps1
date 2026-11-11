<#
.SYNOPSIS
  Read-only check that every emulator download RetroHub pins still exists upstream with the pinned size.

.DESCRIPTION
  Parses the pins from src\hub\emulators.cpp (every `static const Release` declaration, with the
  fritchii_pin block resolved). Each pin is then checked with metadata requests only; nothing is downloaded:
    - github.com pins: gh api repos/<owner>/<repo>/releases/tags/<tag>, find the asset by name, compare
      .size and (when GitHub publishes one) the digest "sha256:..." with the pinned SHA-256.
    - other hosts: HTTP HEAD with User-Agent "pin-check", redirects followed, compare Content-Length.
      A HEAD cannot verify the SHA-256, so those lines say so.
  Hookshot's toolchain pins (src\hub\hookshot_build.cpp, pins()) are not emulator downloads and are not checked here.

  Status per pin: OK, SIZE MISMATCH, SHA MISMATCH, MISSING, or SKIPPED(reason).
  Exit codes: 0 = every pin OK or SKIPPED; 1 = at least one SIZE MISMATCH, SHA MISMATCH or MISSING;
              3 = the parser found fewer pins than declared (nothing is reported as OK in that case).

.PARAMETER Source
  Path to emulators.cpp. Defaults to ..\src\hub\emulators.cpp relative to this script.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File qa\check_pins.ps1
#>
param(
    [string]$Source = ''
)

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Source) { $Source = Join-Path $repoRoot 'src\hub\emulators.cpp' }
if (-not (Test-Path -LiteralPath $Source)) {
    Write-Output "FATAL: cannot read source file: $Source"
    exit 3
}
$rawText = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Source).Path, [System.Text.Encoding]::UTF8)
# Comments are removed first so that commented-out pins cannot be picked up.
$code = [regex]::Replace($rawText, '/\*.*?\*/', ' ', 'Singleline')

# Turns one field of a Release initializer into its value: L"text", "text", 123ull, or a
# namespace constant such as fritchii_pin::kPath (looked up by name in the same file).
function Resolve-Token([string]$tok) {
    $t = $tok.Trim()
    if ($t -match '^L?"(.*)"$') { return $Matches[1] }
    if ($t -match '^(\d+)ull$') { return $Matches[1] }
    if ($t -match '^\w+::(\w+)$') {
        $name = $Matches[1]
        $m = [regex]::Match($code, '\b' + $name + '\s*=\s*(?:L?"([^"]*)"|(\d+)ull)')
        if ($m.Success) {
            if ($m.Groups[1].Success) { return $m.Groups[1].Value }
            return $m.Groups[2].Value
        }
    }
    return $null
}

# gh api for one release. Returns Exit, Out (stdout) and Err (stderr). No window is opened.
function Invoke-GhApi([string]$endpoint) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'gh'
    $psi.Arguments = 'api ' + $endpoint
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $outText = $proc.StandardOutput.ReadToEnd()
    $errText = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    return @{ Exit = $proc.ExitCode; Out = $outText; Err = $errText }
}

# ---------------------------------------------------------------- parse
$declRx = [regex]'static\s+const\s+Release\s+(\w+)\s*=\s*\{([^}]*)\}'
$decls = $declRx.Matches($code)
$looseDecls = ([regex]'\bRelease\s+\w+\s*=').Matches($code).Count
$kPathDefs = ([regex]'\bkPath\s*=').Matches($code).Count

$pins = New-Object System.Collections.ArrayList
$problems = New-Object System.Collections.ArrayList
foreach ($d in $decls) {
    $name = $d.Groups[1].Value
    $f = @($d.Groups[2].Value -split ',')
    if ($f.Count -lt 5) { [void]$problems.Add("${name}: only $($f.Count) fields in initializer"); continue }
    $ver = Resolve-Token $f[0]
    $hostName = Resolve-Token $f[1]
    $pathVal = Resolve-Token $f[2]
    $sizeS = Resolve-Token $f[3]
    $shaVal = Resolve-Token $f[4]
    $exe = ''
    if ($f.Count -gt 5) { $exe = Resolve-Token $f[5] }
    if ($null -eq $ver -or -not $hostName -or -not $pathVal -or -not $sizeS -or -not $shaVal) {
        [void]$problems.Add("${name}: could not resolve every field (namespace constant missing?)"); continue
    }
    if ($sizeS -notmatch '^\d+$') { [void]$problems.Add("${name}: size is not a number: $sizeS"); continue }
    if ($shaVal -notmatch '^[0-9a-fA-F]{64}$') { [void]$problems.Add("${name}: sha256 is not 64 hex digits"); continue }
    [void]$pins.Add([pscustomobject]@{
        Name    = $name
        Version = $ver
        Host    = $hostName
        Path    = $pathVal
        Size    = [uint64]$sizeS
        Sha256  = $shaVal.ToLower()
        Exe     = $exe
        KPath   = ($f[2] -match 'kPath')
    })
}

$kPathPins = @($pins | Where-Object { $_.KPath }).Count
$expected = [Math]::Max($decls.Count, $looseDecls)
if ($pins.Count -lt $expected -or $kPathPins -lt $kPathDefs -or $problems.Count -gt 0) {
    Write-Output "FATAL: parsed $($pins.Count) pins, but emulators.cpp declares $expected Release pins ($kPathDefs kPath definitions, $kPathPins parsed from them)."
    foreach ($p in $problems) { Write-Output "  parse problem: $p" }
    Write-Output 'Refusing to report: fix the parser (qa\check_pins.ps1) or the source before trusting these pins.'
    exit 3
}

Write-Output "Pins found in $Source : $($pins.Count)"
$pins | Select-Object Name, Version, Size, @{ Name = 'Sha256'; Expression = { $_.Sha256.Substring(0, 16) + '...' } }, Host, Exe |
    Format-Table -AutoSize | Out-String -Width 240 | Write-Output

# ---------------------------------------------------------------- check
$ghCache = @{}
$results = New-Object System.Collections.ArrayList

foreach ($p in $pins) {
    $url = 'https://' + $p.Host + $p.Path
    $label = $p.Name + ' ' + $p.Version
    $status = ''
    $detail = ''

    if ($p.Host -ieq 'github.com') {
        $mm = [regex]::Match($p.Path, '^/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$')
        if (-not $mm.Success) {
            $status = 'SKIPPED(unrecognized github path)'
        } else {
            $owner = $mm.Groups[1].Value
            $repoName = $mm.Groups[2].Value
            $tag = $mm.Groups[3].Value
            $assetName = $mm.Groups[4].Value
            $key = "$owner/$repoName@$tag"
            if (-not $ghCache.ContainsKey($key)) {
                $ghCache[$key] = Invoke-GhApi ("repos/$owner/$repoName/releases/tags/$tag")
            }
            $gh = $ghCache[$key]
            if ($gh.Exit -ne 0) {
                $errLine = ($gh.Err -split "`n")[0].Trim()
                if ($gh.Err -match 'HTTP 404') {
                    $status = 'MISSING'
                    $detail = "release tag $tag not found"
                } else {
                    $status = 'SKIPPED(gh api failed: ' + $errLine + ')'
                }
            } else {
                $rel = $gh.Out | ConvertFrom-Json
                $asset = $null
                foreach ($a in @($rel.assets)) { if ($a.name -eq $assetName) { $asset = $a; break } }
                if ($null -eq $asset) {
                    $status = 'MISSING'
                    $detail = "asset $assetName not in release $tag"
                } elseif ([uint64]$asset.size -ne $p.Size) {
                    $status = 'SIZE MISMATCH'
                    $detail = "upstream $($asset.size), pinned $($p.Size)"
                } elseif ($asset.digest -and ($asset.digest -match '^sha256:([0-9a-fA-F]{64})$')) {
                    if ($Matches[1].ToLower() -ne $p.Sha256) {
                        $status = 'SHA MISMATCH'
                        $detail = "upstream $($Matches[1].ToLower()), pinned $($p.Sha256)"
                    } else {
                        $status = 'OK'
                        $detail = 'size and sha256 match GitHub digest'
                    }
                } else {
                    $status = 'OK'
                    $detail = 'size matches; GitHub publishes no digest, sha256 not checked'
                }
            }
        }
    } else {
        try {
            $resp = Invoke-WebRequest -Uri $url -Method Head -Headers @{ 'User-Agent' = 'pin-check' } -UseBasicParsing -MaximumRedirection 10 -TimeoutSec 60
            $cl = $resp.Headers['Content-Length']
            if ($cl -is [array]) { $cl = $cl[0] }
            if (-not $cl) {
                $status = 'SKIPPED(no Content-Length header)'
            } elseif ([uint64]$cl -ne $p.Size) {
                $status = 'SIZE MISMATCH'
                $detail = "upstream $cl, pinned $($p.Size)"
            } else {
                $status = 'OK'
                $detail = 'size matches; sha256 not checked (HEAD only, host publishes no digest)'
            }
        } catch {
            $httpCode = 0
            if ($_.Exception.Response) { $httpCode = [int]$_.Exception.Response.StatusCode }
            $msg = ($_.Exception.Message -split "`n")[0].Trim()
            if ($httpCode -eq 404 -or $httpCode -eq 410) {
                $status = 'MISSING'
                $detail = "HTTP $httpCode"
            } else {
                $status = 'SKIPPED(HEAD failed: ' + $msg + ')'
            }
        }
    }

    [void]$results.Add([pscustomobject]@{ Status = $status; Label = $label; Url = $url; Detail = $detail })
}

# ---------------------------------------------------------------- report
foreach ($r in $results) {
    Write-Output ('{0,-30} {1,-22} {2}' -f $r.Status, $r.Label, $r.Url)
    if ($r.Detail) { Write-Output ('{0,-30} {1}' -f '', $r.Detail) }
}

$bad = @($results | Where-Object { $_.Status -ne 'OK' -and $_.Status -notlike 'SKIPPED*' })
$skipped = @($results | Where-Object { $_.Status -like 'SKIPPED*' })
$okCount = @($results | Where-Object { $_.Status -eq 'OK' }).Count
Write-Output ''
Write-Output "Summary: $okCount OK, $($bad.Count) not OK, $($skipped.Count) SKIPPED, of $($results.Count) pins."
if ($bad.Count -gt 0) { exit 1 }
exit 0
