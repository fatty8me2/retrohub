# Headless screenshots of the Library desk layout (Settings > General > Layout), plus two Traditional shots to compare with another build.
# Runs a SANDBOX COPY of build\bin\Release (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown and nothing
# is launched (spawn() refuses in screenshot mode). Waits while the owner is gaming. One hub at a time; a hub that hangs is ended by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\desk_shots.ps1 [-Out dir] [-Only regex] [-Bin dir] [-Tag name]
#   -Bin  another build's bin\Release (e.g. an origin/main build) for the Traditional comparison shots; -Tag names its shots.
# Output: <name>.png (+ .bmp), <name>.trace (HUB_SHOT_TRACE lines: desk-model / desk-cursor / desk-play ...), index.txt.
param([string]$Out = '', [string]$Only = '', [string]$Bin = '', [string]$Tag = 'desk')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_desk_out' }
$sandbox = Join-Path $root ("build\qa_" + $Tag)
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
& robocopy $Bin $sandbox /MIR /XD userdata /NFL /NDL /NJH /NJS /NP | Out-Null   # the sandbox copy (never the build folder itself)
$exe = Join-Path $sandbox 'RetroHub.exe'
if (-not (Test-Path $exe)) { throw "no RetroHub.exe in $Bin" }
Add-Type -AssemblyName System.Drawing

$walk = 'down,down,down,down,down,down,down,down,down,down,down,down,down,down,shot:walk,right,shot:pane,b,shot:back,left,shot:header,left,shot:closed'
$shots = @(
    @{ n = 'desk-all-1280';     v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-all-1024';     v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1024x600' } },
    @{ n = 'desk-all-1920';     v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1920x1080' } },
    @{ n = 'desk-14-1280';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-60-big';       v = @{ HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_TITLES = '1'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-empty-1280';   v = @{ HUB_SHOT_FAKE_LIB = '0'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-empty-1024';   v = @{ HUB_SHOT_FAKE_LIB = '0'; HUB_SHOT_SIZE = '1024x600' } },
    @{ n = 'desk-search';       v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SEARCH = 'mario'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-search-none';  v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_SEARCH = 'zzzz'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-walk';         v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = $walk } },
    @{ n = 'desk-walk-1024';    v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1024x600'; HUB_SHOT_PAD = $walk } },
    @{ n = 'desk-rt-page';      v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'rt,shot:rt1,rt,shot:rt2,lt' } },
    @{ n = 'desk-play-a';       v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,down,a' } },
    @{ n = 'desk-options-y';    v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,y' } },
    @{ n = 'desk-friends';      v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-friends-1920'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_SIZE = '1920x1080' } },
    @{ n = 'desk-scale150';     v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_UI_SCALE = '150'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-scale150-1920'; v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_UI_SCALE = '150'; HUB_SHOT_SIZE = '1920x1080' } },
    @{ n = 'desk-light';        v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_THEME = 'light'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-gallery';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_GALLERY = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-settings';     v = @{ HUB_SHOT_PAGE = 'settings:general'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_WHEEL = '1180,640,22' } },
    @{ n = 'desk-pane-left';    v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,right,right,shot:online,left,left,shot:list,up,up,up,shot:search,down,shot:back,rb,shot:rb,lb' } },
    @{ n = 'desk-click';        v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_CLICK = '250,158;250,306' } },
    @{ n = 'desk-wheel';        v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_WHEEL = '250,400,4' } },
    @{ n = 'desk-keys';         v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_SECS = '7'; HUB_SHOT_KEYS = 'down,down,end,pgup,left,left' } },
    @{ n = 'desk-palette';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PALETTE = 'test game 5' } },
    @{ n = 'desk-bell';         v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_BELL = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-discover';    v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_PAGE = 'discover'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'trad-library-14';   v = @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'trad-library-14-1920'; v = @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1920x1080' } },
    @{ n = 'trad-explicit-14';  v = @{ HUB_SHOT_LAYOUT = 'traditional'; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720' } }
)

$index = @()
foreach ($s in $shots) {
    if ($Only -and $s.n -notmatch $Only) { continue }
    if ($Tag -ne 'desk' -and $s.n -notlike 'trad-library-*') { continue }   # another build: only the Traditional comparison shots
    Wait-NoGame
    Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
    Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue   # every shot starts from a fresh install
    $vault = Join-Path $sandbox 'vault'
    Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
    $env:RETROHUB_QA = '1'; $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
    $env:HUB_SHOT_VAULT = $vault
    $env:HUB_SHOT_SECS = '5'
    $env:HUB_SHOT_LAYOUT = 'desk'
    foreach ($k in $s.v.Keys) { if ($s.v[$k] -eq '') { Remove-Item ("env:" + $k) -ErrorAction SilentlyContinue } else { Set-Item ("env:" + $k) $s.v[$k] } }
    $name = if ($Tag -eq 'desk') { $s.n } else { "$($s.n)-$Tag" }
    $bmp = Join-Path $Out "$name.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $env:HUB_SHOT_TRACE = Join-Path $Out "$name.trace"
    Remove-Item -Force $bmp, $env:HUB_SHOT_TRACE -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "${name}: timed out (ended PID $($p.Id))" }
    $p.WaitForExit()
    $code = $p.ExitCode
    foreach ($b in Get-ChildItem $Out -Filter "$name*.bmp") {   # the shot plus any shot:<name> frames of a pad walk
        $img = [System.Drawing.Image]::FromFile($b.FullName)
        $img.Save([IO.Path]::ChangeExtension($b.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
    }
    $line = "$name | exit $code | " + (($s.v.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
    Write-Host $line
    $index += $line
}
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
$index | Set-Content -Encoding utf8 (Join-Path $Out "index-$Tag.txt")


