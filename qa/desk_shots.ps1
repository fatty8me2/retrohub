# Headless screenshots of the Library desk layout (Settings > General > Layout), plus Traditional shots to compare with another build.
# Runs a SANDBOX COPY of build\bin\Release (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown and nothing
# is launched (spawn() refuses in screenshot mode). Waits while the owner is gaming. One hub at a time; a hub that hangs is ended by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\desk_shots.ps1 [-Out dir] [-Only regex] [-Bin dir] [-Tag name]
#   -Bin  another build's bin\Release (e.g. a build of the base commit) for the Traditional comparison shots (base = $true); -Tag names
#         them. Compare <name>.png with <name>-<tag>.png: Traditional must be pixel-identical except the one-time "Try" card.
# A shot may pre-write the sandbox's userdata\hub.json (hub = JSON text: setup finished, the card answered, a list width, a remembered
# cursor ...) and may give the hub friends' games (ghosts = $true: a fixture of friends' libraries through HUB_CASES_AUDIT_FRIENDS).
# Output: <name>.png (+ .bmp), <name>.trace (HUB_SHOT_TRACE lines: desk-model / desk-cursor / desk-play / desk-sort ...), index-<tag>.txt.
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
$friends = Join-Path $sandbox 'desk_friend_libs.json'   # friends' libraries for the friends' games shots (headless only)
Set-Content -Encoding ascii $friends '{"libs": {"miauid": {"games": [{"s": "n64", "k": "n64:deskghost1", "c": "", "t": "Banjo-Kazooie"}, {"s": "gc", "k": "gc:deskghost2", "c": "", "t": "Super Mario Sunshine"}, {"s": "ps2", "k": "ps2:deskghost3", "c": "", "t": "Okami"}]}}}'

$walk = 'down,down,down,down,down,down,down,down,down,down,down,down,down,down,shot:walk,right,shot:pane,b,shot:back,left,shot:header,left,shot:closed'
$top = 'lt,lt,lt,lt,up'   # from the list: its first row, then Up onto the toolbar (the Sort chip)
$quiet = '"show_friend_games": false, "friend_games_v": 1'   # no greyed pack games: the shared pack listing arrives at a random moment
$setup = '"setup_done": true, "username_claimed": true, "net_probe_done": true'   # every setup step done: no Finish setup card
$done = "{""setup_done"": true, ""desk_card_done"": true, $quiet}"   # setup finished, the Try card answered: Traditional as before
$a14 = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_FAKE_ART = '1' }
function V($base, $more) { $h = @{}; foreach ($k in $base.Keys) { $h[$k] = $base[$k] }; foreach ($k in $more.Keys) { $h[$k] = $more[$k] }; return $h }
$all = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_FAKE_ART = '1' }
$shots = @(
    @{ n = 'desk-all-1280';     v = (V $all @{ HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-all-1024';     v = (V $all @{ HUB_SHOT_SIZE = '1024x600' }) },
    @{ n = 'desk-all-1920';     v = (V $all @{ HUB_SHOT_SIZE = '1920x1080' }) },
    @{ n = 'desk-14-1280';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-60-big';       v = @{ HUB_SHOT_FAKE_LIB = '60'; HUB_SHOT_FAKE_TITLES = '1'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_FAKE_ART = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-empty-1280';   v = @{ HUB_SHOT_FAKE_LIB = '0'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-empty-1024';   v = @{ HUB_SHOT_FAKE_LIB = '0'; HUB_SHOT_SIZE = '1024x600' } },
    @{ n = 'desk-native-1280';  v = @{ HUB_SHOT_FAKE_LIB = 'native'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-done-1280';    v = (V $all @{ HUB_SHOT_SIZE = '1280x720' }); hub = $done },
    @{ n = 'desk-search';       v = (V $all @{ HUB_SHOT_SEARCH = 'mario'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-search-none';  v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_SEARCH = 'zzzz'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-walk';         v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = $walk }) },
    @{ n = 'desk-walk-1024';    v = (V $all @{ HUB_SHOT_SIZE = '1024x600'; HUB_SHOT_PAD = $walk }) },
    @{ n = 'desk-walk-150';     v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_UI_SCALE = '150'; HUB_SHOT_PAD = $walk }) },
    @{ n = 'desk-rt-page';      v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'rt,shot:rt1,rt,shot:rt2,lt' } },
    @{ n = 'desk-play-a';       v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,down,a' } },
    @{ n = 'desk-options-y';    v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,y' } },
    @{ n = 'desk-friends';      v = (V $all @{ HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-friends-1920'; v = (V $all @{ HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_SIZE = '1920x1080' }) },
    @{ n = 'desk-ghosts';       v = (V $a14 @{ HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_SEARCH = 'o'; HUB_SHOT_PAD = 'shot:list' }); ghosts = $true },
    @{ n = 'desk-ghost-open';   v = (V $a14 @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_DESK_TYPE = 'b'; HUB_SHOT_PAD = 'wait,wait,wait,wait,shot:row,right,shot:card,left,a,shot:view' }); ghosts = $true },
    @{ n = 'desk-ghost-trad';   v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720' }); ghosts = $true },
    @{ n = 'desk-dupes';        v = (V $a14 @{ HUB_SHOT_FAKE_DUPES = '1'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-hidden';       v = (V $a14 @{ HUB_SHOT_HIDDEN = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = "$top,right,right,right,shot:morefocus,a,shot:more,down,down,shot:hiddenitem,a,shot:hiddenlist" }) },
    @{ n = 'desk-sort';         v = (V $a14 @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = "$top,shot:sortfocus,a,shot:sort1,a,shot:sort2,x,shot:sortrev" }) },
    @{ n = 'desk-filter';       v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = "$top,right,shot:filterfocus,a,shot:filtermenu,a,shot:favsonly" }) },
    @{ n = 'desk-genre';        v = (V $a14 @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = "$top,right,a,down,down,shot:genreitem,right,shot:genresub,down,a,shot:genre1" }) },
    @{ n = 'desk-filter-none';  v = (V $a14 @{ HUB_SHOT_SIZE = '1024x600'; HUB_SHOT_LIBFILTER = 'genre=Nothing like this' }) },
    @{ n = 'desk-groups';       v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = "$top,right,right,right,a,down,shot:closeall,a,shot:allclosed" }) },
    @{ n = 'desk-jump';         v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_SECS = '8'; HUB_SHOT_KEYS = 'p,p,p,z,e' }) },
    @{ n = 'desk-jump-chars';   v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_DESK_TYPE = 'mk' }) },
    @{ n = 'desk-search-slash'; v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_DESK_TYPE = '/ma' }) },
    @{ n = 'desk-coverflow';    v = (V $a14 @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,down,down,rt,rt,rt,rt,rt,down,shot:foot,right,shot:cffocus,a,shot:cf,right,right,shot:cf2,b,shot:back' }) },
    @{ n = 'desk-drag';         v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_DESK_DRAG = '120' }) },
    @{ n = 'desk-drag-back';    v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_DESK_DRAG = '-400' }) },
    @{ n = 'desk-narrow-1024';  v = (V $all @{ HUB_SHOT_SIZE = '1024x600' }); hub = "{""ui_layout"": ""desk"", $quiet, ""desk_list_w"": 30}" },
    @{ n = 'desk-wide-1024';    v = (V $all @{ HUB_SHOT_SIZE = '1024x600' }); hub = "{""ui_layout"": ""desk"", $quiet, ""desk_list_w"": 60}" },
    @{ n = 'desk-wide-1920';    v = (V $all @{ HUB_SHOT_SIZE = '1920x1080' }); hub = "{""ui_layout"": ""desk"", $quiet, ""desk_list_w"": 60}" },
    @{ n = 'desk-restore';      v = (V $all @{ HUB_SHOT_SIZE = '1280x720' }); hub = "{""ui_layout"": ""desk"", $quiet, ""desk_sel"": ""nds|fake:nds-plain"", ""desk_scroll"": 21.9}" },
    @{ n = 'desk-pages';        v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,down,down,down,down,down,down,down,shot:before,rb,shot:online,lb,shot:after' }) },
    @{ n = 'desk-scale150';     v = (V $all @{ HUB_UI_SCALE = '150'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-scale150-1920'; v = (V $all @{ HUB_UI_SCALE = '150'; HUB_SHOT_SIZE = '1920x1080' }) },
    @{ n = 'desk-scale150-1024'; v = (V $all @{ HUB_UI_SCALE = '150'; HUB_SHOT_SIZE = '1024x600' }) },
    @{ n = 'desk-light';        v = (V $all @{ HUB_THEME = 'light'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'desk-light-filter'; v = (V $a14 @{ HUB_THEME = 'light'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_LIBFILTER = 'fav' }) },
    @{ n = 'desk-pane-play-1024'; v = (V $all @{ HUB_SHOT_SIZE = '1024x600'; HUB_SHOT_DETAILS = 'fake:n64-online' }); hub = "{$setup, $quiet}" },
    @{ n = 'desk-pane-play-150';  v = (V $all @{ HUB_SHOT_SIZE = '1280x720'; HUB_UI_SCALE = '150'; HUB_SHOT_DETAILS = 'fake:n64-online' }); hub = "{$setup, $quiet}" },
    @{ n = 'desk-gallery';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_GALLERY = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-settings';     v = @{ HUB_SHOT_PAGE = 'settings:general'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_WHEEL = '1180,640,22' } },
    @{ n = 'desk-pane-left';    v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'down,right,right,shot:online,left,left,shot:list,up,up,up,shot:search,down,shot:back,rb,shot:rb,lb' } },
    @{ n = 'desk-click';        v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_CONTINUE = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_CLICK = '250,198;250,346' } },
    @{ n = 'desk-wheel';        v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_WHEEL = '250,400,4' } },
    @{ n = 'desk-keys';         v = @{ HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_FAKE_AZ = '1'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_SECS = '7'; HUB_SHOT_KEYS = 'down,down,end,pgup,left,left' } },
    @{ n = 'desk-palette';      v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PALETTE = 'test game 5' } },
    @{ n = 'desk-bell';         v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_FAKE_FRIENDS = '1'; HUB_SHOT_FRIENDS = '1'; HUB_SHOT_BELL = '1'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-discover';     v = @{ HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_PAGE = 'discover'; HUB_SHOT_SIZE = '1280x720' } },
    @{ n = 'desk-note';         v = (V $a14 @{ HUB_SHOT_SIZE = '1280x720' }); hub = "{""ui_layout"": ""desk"", $quiet, ""setup_done"": true, ""desk_card_done"": true, ""desk_switch_note"": true}" },
    # Traditional: the one-time card (setup finished, not answered yet), Try it / Not now by pad, and the comparison shots (base = also on -Bin)
    @{ n = 'trad-card-1280';    v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-card-1024';    v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1024x600' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-card-light';   v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_THEME = 'light'; HUB_SHOT_SIZE = '1920x1080' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-card-try';     v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'up,left,shot:focus,a,shot:desk' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-card-notnow';  v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_PAD = 'up,shot:focus,a,shot:gone' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-card-new';     v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720' }) },
    @{ n = 'trad-card-playing'; v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720'; HUB_SHOT_FAKE_RUNNING = '1' }); hub = "{""setup_done"": true, $quiet}" },
    @{ n = 'trad-library-14';   v = @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720' }; base = $true },
    @{ n = 'trad-library-14-1920'; v = @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1920x1080' }; base = $true },
    @{ n = 'trad-library-done'; v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1280x720' }); hub = $done; base = $true },
    @{ n = 'trad-library-done-1024'; v = (V $a14 @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_SIZE = '1024x600' }); hub = $done; base = $true },
    @{ n = 'trad-library-done-all'; v = @{ HUB_SHOT_LAYOUT = ''; HUB_SHOT_PAGE = 'library'; HUB_SHOT_LIBVIEW = 'grid'; HUB_SHOT_FAKE_LIB = 'all'; HUB_SHOT_SIZE = '1280x720' }; hub = $done; base = $true },
    @{ n = 'trad-explicit-14';  v = @{ HUB_SHOT_LAYOUT = 'traditional'; HUB_SHOT_PAGE = 'library'; HUB_SHOT_FAKE_LIB = '14'; HUB_SHOT_SIZE = '1280x720' } }
)

$index = @()
foreach ($s in $shots) {
    if ($Only -and $s.n -notmatch $Only) { continue }
    if ($Tag -ne 'desk' -and -not $s.base) { continue }   # another build: only the Traditional comparison shots
    Wait-NoGame
    Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
    Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue   # every shot starts from a fresh install
    $hub = if ($s.hub) { $s.hub } elseif ($s.ghosts -or $s.friendsOn) { '' } else { "{$quiet}" }
    if ($hub) {
        New-Item -ItemType Directory -Force (Join-Path $sandbox 'userdata') | Out-Null
        Set-Content -Encoding ascii (Join-Path $sandbox 'userdata\hub.json') $hub
    }
    $vault = Join-Path $sandbox 'vault'
    Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
    $env:RETROHUB_QA = '1'; $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
    $env:HUB_SHOT_VAULT = $vault
    $env:HUB_SHOT_SECS = '5'
    $env:HUB_SHOT_LAYOUT = 'desk'
    if ($s.ghosts) { $env:HUB_CASES_AUDIT_FRIENDS = $friends } else { Remove-Item env:HUB_CASES_AUDIT_FRIENDS -ErrorAction SilentlyContinue }
    foreach ($k in $s.v.Keys) { if ($s.v[$k] -eq '') { Remove-Item ("env:" + $k) -ErrorAction SilentlyContinue } else { Set-Item ("env:" + $k) $s.v[$k] } }
    $name = if ($Tag -eq 'desk') { $s.n } else { "$($s.n)-$Tag" }
    $bmp = Join-Path $Out "$name.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $env:HUB_SHOT_TRACE = Join-Path $Out "$name.trace"
    Get-ChildItem $Out -Filter "$name*.bmp" -ErrorAction SilentlyContinue | Where-Object { $_.BaseName -eq $name -or $_.BaseName -like "$name-*" } | Remove-Item -Force
    Remove-Item -Force $env:HUB_SHOT_TRACE -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "${name}: timed out (ended PID $($p.Id))" }
    $p.WaitForExit()
    $code = $p.ExitCode
    foreach ($b in Get-ChildItem $Out -Filter "$name*.bmp") {   # the shot plus any shot:<name> frames of a pad walk
        if ($b.BaseName -ne $name -and $b.BaseName -notlike "$name-*") { continue }
        $img = [System.Drawing.Image]::FromFile($b.FullName)
        $img.Save([IO.Path]::ChangeExtension($b.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
    }
    $line = "$name | exit $code | " + (($s.v.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ') + $(if ($hub) { " | hub.json $hub" } else { '' })
    Write-Host $line
    $index += $line
}
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
$index | Set-Content -Encoding utf8 (Join-Path $Out "index-$Tag.txt")
