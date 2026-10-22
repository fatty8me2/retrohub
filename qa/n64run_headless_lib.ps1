# Helpers for headless n64run runs (dot-source this file). Windows PowerShell 5.1.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class WinProbe {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    // Visible top-level windows owned by pid (a hidden SDL window is not counted).
    public static int VisibleTopLevel(int pid) {
        int n = 0;
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == (uint)pid && IsWindowVisible(h)) n++; return true; }, IntPtr.Zero);
        return n;
    }
}
'@

# Starts n64run with the given argument string (no window: -WindowStyle Hidden + --headless), polls for
# visible top-level windows while it runs. Returns @{ Proc; Exit; MaxVisible; Out; Err }.
function Start-HeadlessRun([string]$Exe, [string]$ArgString, [string]$OutBase) {
    $env:N64RUN_MUTE = '1'; $env:SSB64_MUTE = '1'
    $p = Start-Process -FilePath $Exe -ArgumentList $ArgString -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$OutBase.out.txt" -RedirectStandardError "$OutBase.err.txt"
    $null = $p.Handle   # keep the handle so ExitCode is readable after exit
    return $p
}

function Wait-HeadlessRun($p, [string]$OutBase, [int]$TimeoutSec = 300) {
    $max = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        $v = [WinProbe]::VisibleTopLevel($p.Id)
        if ($v -gt $max) { $max = $v }
        Start-Sleep -Milliseconds 100
    }
    $timedOut = -not $p.HasExited
    if ($timedOut) { try { $p.Kill() } catch {} }
    $p.WaitForExit()
    $null = $p.ExitCode
    return @{ Exit = $(if ($timedOut) { -1 } else { $p.ExitCode }); TimedOut = $timedOut; MaxVisible = $max;
              Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Err = (Get-Content "$OutBase.err.txt" -ErrorAction SilentlyContinue) }
}

# Waits for several runs at once (two-peer netplay). $items = @(@{P=proc; Base=outbase}, ...). Returns one result per item.
function Wait-HeadlessRuns($items, [int]$TimeoutSec = 300) {
    $max = @(0) * $items.Count
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec -and ($items | Where-Object { -not $_.P.HasExited })) {
        for ($i = 0; $i -lt $items.Count; $i++) {
            if (-not $items[$i].P.HasExited) { $v = [WinProbe]::VisibleTopLevel($items[$i].P.Id); if ($v -gt $max[$i]) { $max[$i] = $v } }
        }
        Start-Sleep -Milliseconds 100
    }
    $res = @()
    for ($i = 0; $i -lt $items.Count; $i++) {
        $p = $items[$i].P
        $to = -not $p.HasExited
        if ($to) { try { $p.Kill() } catch {} }
        $p.WaitForExit()
        $res += @{ Exit = $(if ($to) { -1 } else { $p.ExitCode }); TimedOut = $to; MaxVisible = $max[$i];
                   Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Err = (Get-Content "$($items[$i].Base).err.txt" -ErrorAction SilentlyContinue) }
    }
    return $res
}
