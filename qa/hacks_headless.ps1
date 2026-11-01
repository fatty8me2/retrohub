# ROM hack end-to-end, headless and muted (Plan 4 H2-H4): make a synthetic BPS for a test ROM, add it through the real add path
# (hub_hack_test --add: inspect, find the base by CRC, build the patched image, write hack.json into a scratch vault), check the
# key == CRC32 of the image, boot the image with retrorun --headless --screenshot and check that its saves land in the HACK's own vault folder.
# Usage: qa\hacks_headless.ps1 [-Root <worktree>]
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
$bin = Join-Path $Root 'build\bin\Release'
$exe = Join-Path $bin 'retrorun.exe'
$tool = Join-Path $bin 'hub_hack_test.exe'
$core = Join-Path $Root 'deps\cores\mgba_libretro.dll'
$rom = Join-Path $Root 'deps\testroms\sram.gba'
$work = Join-Path $env:TEMP ('hacks_qa_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force "$work\vault", "$work\cache" | Out-Null
$fail = 0
function Fail($m) { Write-Host "FAIL: $m"; $script:fail++ }

Add-Type -TypeDefinition @'
using System; using System.Collections.Generic;
public static class Bps {
  static uint[] T; static Bps() { T = new uint[256]; for (uint i = 0; i < 256; i++) { uint c = i; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; T[i] = c; } }
  public static uint Crc(byte[] d, int n) { uint c = 0xFFFFFFFFu; for (int i = 0; i < n; i++) c = T[(c ^ d[i]) & 0xFF] ^ (c >> 8); return ~c; }
  static void Var(List<byte> o, ulong n) { for (;;) { byte x = (byte)(n & 0x7f); n >>= 7; if (n == 0) { o.Add((byte)(x | 0x80)); break; } o.Add(x); n--; } }
  static void U32(List<byte> o, uint v) { for (int i = 0; i < 4; i++) o.Add((byte)(v >> (8 * i))); }
  // same-size patch: byte `pos` is replaced with `val`
  public static byte[] Make(byte[] src, int pos, byte val) {
    byte[] tgt = (byte[])src.Clone(); tgt[pos] = val;
    var o = new List<byte>(new byte[] { (byte)'B', (byte)'P', (byte)'S', (byte)'1' });
    Var(o, (ulong)src.Length); Var(o, (ulong)tgt.Length); Var(o, 0);
    if (pos > 0) Var(o, ((ulong)(pos - 1) << 2) | 0);                 // SourceRead pos bytes
    Var(o, 0UL << 2 | 1); o.Add(val);                                  // TargetRead 1 byte
    if (src.Length - pos - 1 > 0) Var(o, ((ulong)(src.Length - pos - 2) << 2) | 0);   // SourceRead the rest
    U32(o, Crc(src, src.Length)); U32(o, Crc(tgt, tgt.Length));
    byte[] a = o.ToArray(); U32(o, Crc(a, a.Length));
    return o.ToArray();
  }
}
'@

$src = [IO.File]::ReadAllBytes($rom)
$pos = $src.Length - 4; $val = [byte](($src[$pos] + 1) -band 0xFF)
$patch = Join-Path $work 'qa hack (v1).bps'
[IO.File]::WriteAllBytes($patch, [Bps]::Make($src, $pos, $val))

# 1. the add path
$out = & $tool --add $rom $patch gba "$work\vault" "$work\cache"
$out | ForEach-Object { Write-Host "  $_" }
$key = ($out | Where-Object { $_ -like 'KEY=*' }) -replace '^KEY=', ''
$img = ($out | Where-Object { $_ -like 'IMAGE=*' }) -replace '^IMAGE=', ''
$gdir = ($out | Where-Object { $_ -like 'GAMEDIR=*' }) -replace '^GAMEDIR=', ''
if (-not $key -or -not (Test-Path $img)) { Fail 'add path produced no image'; Remove-Item -Recurse -Force $work; exit 1 }
if (-not (Test-Path "$gdir\patch\hack.json")) { Fail 'hack.json missing in the vault' }
if (-not (Test-Path "$gdir\patch\qa hack (v1).bps")) { Fail 'patch file missing next to hack.json' }

# 2. key == CRC32 of the patched image, and it is the patched image
$bytes = [IO.File]::ReadAllBytes($img)
$crc = '{0:x8}' -f [Bps]::Crc($bytes, $bytes.Length)
if (-not $key.EndsWith("+h$crc")) { Fail "key $key does not end with the image CRC $crc" }
if ($bytes[$pos] -ne $val -or $bytes.Length -ne $src.Length) { Fail 'the image is not the patched game' }
if ([Bps]::Crc($src, $src.Length) -eq [Bps]::Crc($bytes, $bytes.Length)) { Fail 'the image equals the original' }

# 3. boot it headless and screenshot; saves go to the hack's own vault folder
$sv = "$gdir\battery"; $st = "$gdir\states"
New-Item -ItemType Directory -Force $sv, $st | Out-Null
$shot = Join-Path $work 'hack.png'
& $exe --core $core --rom $img --system gba --save-dir $sv --state-dir $st --headless --frames 240 --screenshot $shot 2>&1 | Out-Null
if (-not (Test-Path $shot) -or (Get-Item $shot).Length -lt 200) { Fail 'no screenshot from the patched game' }
$saves = @(Get-ChildItem $sv -File -ErrorAction SilentlyContinue)
Write-Host "  battery files: $($saves.Count) in $sv"
if ($saves.Count -eq 0) { Write-Host '  (this test ROM wrote no save file; the folder was still the runner''s only save target)' }
$stray = @(Get-ChildItem "$work\vault" -Recurse -File | Where-Object { $_.FullName -notlike "$gdir*" })
if ($stray.Count) { Fail "files outside the hack's folder: $($stray.FullName -join ', ')" }

# 4. a patch for another version is refused in plain words and adds nothing
$other = $src.Clone(); $other[100] = [byte](($other[100] + 1) -band 0xFF)
$wrongrom = Join-Path $work 'other.gba'; [IO.File]::WriteAllBytes($wrongrom, $other)
$o2 = & $tool --add $wrongrom $patch gba "$work\vault2" "$work\cache2" 2>&1
if (($o2 -join ' ') -notmatch 'different version') { Fail "wrong-version message missing: $o2" }

Remove-Item -Recurse -Force $work
if ($fail) { exit 1 } else { Write-Host 'hacks_headless: ok' }
