# Headless screenshots of the game page in both layouts: the full-window page (Traditional) and the Library desk's right-hand pane.
# Runs a SANDBOX COPY of a build's bin\Release (no userdata) with a scratch vault, muted, SDL dummy driver: no window is ever shown and
# nothing is launched (spawn() refuses in screenshot mode). Waits while the owner is gaming. One hub at a time; a hub that hangs is ended
# by its PID.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File qa\gamepage_shots.ps1 [-Out dir] [-Only regex] [-Bin dir] [-Tag name] [-Set all|base]
#   -Bin  a build's bin\Release (default: this worktree's); -Tag names the sandbox (build\qa_<Tag>) and the shot names
#   -Set  base = the small set that also runs on an older build (the "before" shots)
# Output: <name>.png (+ .bmp), <name>.trace (HUB_SHOT_TRACE: pad lines, act lines, "captured ... details=yes"), index-<Tag>.txt.
param([string]$Out = '', [string]$Only = '', [string]$Bin = '', [string]$Tag = 'gp', [ValidateSet('all', 'base')][string]$Set = 'all')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot '_gamecheck.ps1')
if (-not $Bin) { $Bin = Join-Path $root 'build\bin\Release' }
if (-not $Out) { $Out = Join-Path $root 'build\qa_gamepage_out' }
$sandbox = Join-Path $root ("build\qa_" + $Tag)
New-Item -ItemType Directory -Force $Out | Out-Null
Wait-NoGame
if ((Resolve-Path $Bin).Path -ne (Resolve-Path $sandbox -ErrorAction SilentlyContinue).Path) {
    & robocopy $Bin $sandbox /MIR /XD userdata vault /NFL /NDL /NJH /NJS /NP | Out-Null   # the sandbox copy (never the build folder itself)
}
$exe = Join-Path $sandbox 'RetroHub.exe'
if (-not (Test-Path $exe)) { throw "no RetroHub.exe in $sandbox" }
Add-Type -AssemblyName System.Drawing

$lib = @{ HUB_SHOT_FAKE_LIB = '14' }
$oot = @{ HUB_SHOT_FAKE_LIB = '1'; HUB_SHOT_HOOKSHOT = 'idle' }
$sizes = @(@('1024', '1024x600', @{}), @('1280', '1280x720', @{}), @('1920', '1920x1080', @{}), @('s150', '1920x1080', @{ HUB_UI_SCALE = '150' }),
           @('s150-1280', '1280x720', @{ HUB_UI_SCALE = '150' }), @('light', '1280x720', @{ HUB_THEME = 'light' }))
$shots = @()
function Add-Shot($n, $layout, $size, $vars, $base = $false) {
    $script:shots += @{ n = $n; layout = $layout; size = $size; v = $vars; base = $base }
}
function Merge($a, $b) { $r = @{}; foreach ($k in $a.Keys) { $r[$k] = $a[$k] }; foreach ($k in $b.Keys) { $r[$k] = $b[$k] }; return $r }

foreach ($layout in @('trad', 'desk')) {
    foreach ($sz in $sizes) {
        $stag = $sz[0]; $size = $sz[1]; $extra = $sz[2]   # (not $tag: PowerShell names are case-insensitive, $Tag is the parameter)
        $isBase = ($layout -eq 'trad' -and $stag -in @('1024', '1280', '1920', 's150'))
        foreach ($t in 0..3) { Add-Shot "$layout-$stag-fake2-tab$t" $layout $size (Merge (Merge $lib $extra) @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_TAB = "$t" }) ($isBase -and $t -in 0, 3) }
        Add-Shot "$layout-$stag-playtime" $layout $size (Merge (Merge $lib $extra) @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PLAYTIME = '1' }) $isBase
        Add-Shot "$layout-$stag-states" $layout $size (Merge (Merge $lib $extra) @{ HUB_SHOT_DETAILS = 'fake:1'; HUB_SHOT_STATES = '1'; HUB_SHOT_TAB = '1' }) $isBase
        foreach ($t in 0..3) { Add-Shot "$layout-$stag-oot-tab$t" $layout $size (Merge (Merge $oot $extra) @{ HUB_SHOT_TAB = "$t" }) ($isBase -and $t -in 0, 3) }
    }
}
# pad walks: Traditional (Y opens the page from the library; RB through the tabs; down into the Settings rows until the body scrolls; B closes)
$down = (1..14 | ForEach-Object { 'down' }) -join ','
Add-Shot 'trad-walk' 'trad' '1280x720' (Merge $lib @{ HUB_SELECT_GAME = 'fake:2'; HUB_SHOT_PAD = "y,shot:open,rb,shot:rb1,rb,rb,shot:settings,$down,shot:scrolled,lt,shot:lt,b,shot:closed" }) $true
Add-Shot 'trad-more' 'trad' '1280x720' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PAD = 'right,right,a,shot:menu,down,shot:menu2,b,shot:menu_closed,b,shot:page_closed' })
Add-Shot 'trad-more-1024' 'trad' '1024x600' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PAD = 'right,right,a,shot:menu' })
Add-Shot 'trad-more-s150' 'trad' '1280x720' (Merge $lib @{ HUB_UI_SCALE = '150'; HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PAD = 'right,right,a,shot:menu' })
# More menu of fake:2 (GameCube): Favorite, Save folder, Cheats, Controls, Add to desktop, Customize, Add to shelf..., Hide game
Add-Shot 'trad-customize' 'trad' '1280x720' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PAD = 'right,right,a,down,down,down,down,down,shot:menu_at,a,shot:customize,b,shot:after_b' })
Add-Shot 'trad-shelf' 'trad' '1280x720' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_PAD = 'right,right,a,down,down,down,down,down,down,a,shot:shelf,b,shot:after_b' })
# Library desk: from the list into the pane (Play), RB through the tabs, down the Settings rows until the body scrolls, B back to the list
Add-Shot 'desk-walk' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = "down,down,shot:list,right,shot:pane,rb,shot:rb1,rb,rb,shot:settings,$down,shot:scrolled,b,shot:back,down,shot:next_game" })
Add-Shot 'desk-walk-1920' 'desk' '1920x1080' (Merge $lib @{ HUB_SHOT_PAD = "down,down,right,rb,rb,rb,shot:settings,$down,shot:scrolled,b,shot:back" })
Add-Shot 'desk-y' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = 'down,y,shot:y' })
Add-Shot 'desk-left' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = 'down,right,shot:pane,left,shot:list,right,right,right,a,left,shot:menu_left,b,shot:menu_closed' })   # Left from Play: the list; Left in the More menu stays in it
Add-Shot 'desk-more' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = 'down,right,right,right,a,shot:menu,b,shot:menu_closed' })
Add-Shot 'desk-customize' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = 'down,right,right,right,a,down,down,down,down,down,shot:menu_at,a,shot:customize,b,shot:after_b' })
Add-Shot 'desk-shelf' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAD = 'down,right,right,right,a,down,down,down,down,down,down,a,shot:shelf,b,shot:after_b' })
Add-Shot 'desk-keeptab' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_TAB = '1'; HUB_SHOT_PAD = 'down,shot:a,down,shot:b,down,shot:c' })
Add-Shot 'desk-palette' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PALETTE = 'test game 5' })
Add-Shot 'trad-overlay-from-online' 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_PAGE = 'online'; HUB_SHOT_DETAILS = 'fake:3' })
# Popups opened from the page, in both hosts: each must open over the page and give the page back when it closes (B / Back)
foreach ($layout in @('trad', 'desk')) {
    if ($layout -eq 'trad') {   # (the desk's pane shows a game before HUB_SHOT_DETAILS lands, so the viewer aid fires too early there: pop-gallery opens it by pad)
        Add-Shot "$layout-pop-viewer" $layout '1280x720' (Merge $lib @{ HUB_SHOT_GALLERY = '1'; HUB_SHOT_VIEWER = '1'; HUB_SHOT_DETAILS = 'fake:0'; HUB_SHOT_TAB = '2'; HUB_SHOT_PAD = 'wait,shot:viewer,b,shot:after_b' })
    }
    Add-Shot "$layout-pop-gallery" $layout '1280x720' (Merge $lib @{ HUB_SHOT_GALLERY = '1'; HUB_SHOT_DETAILS = 'fake:0'; HUB_SHOT_TAB = '2'; HUB_SHOT_PAD = 'down,down,down,shot:first,right,right,right,right,right,shot:last,a,shot:viewer,b,shot:after_b' })   # tab, Open folder (never pressed: headless it only traces), first picture
    Add-Shot "$layout-pop-statedel" $layout '1280x720' (Merge $lib @{ HUB_SHOT_STATES = '1'; HUB_SHOT_DETAILS = 'fake:1'; HUB_SHOT_TAB = '1'; HUB_SHOT_PAD = 'down,down,down,down,down,shot:at,a,shot:confirm,a,shot:deleted' })
    Add-Shot "$layout-pop-stateresume" $layout '1280x720' (Merge $lib @{ HUB_SHOT_STATES = '1'; HUB_SHOT_DETAILS = 'fake:1'; HUB_SHOT_TAB = '1'; HUB_SHOT_PAD = 'down,down,down,down,down,left,shot:at,a,shot:resumed' })
    Add-Shot "$layout-pop-history" $layout '1280x720' (Merge $lib @{ HUB_SHOT_STATES = '1'; HUB_SHOT_DETAILS = 'fake:1'; HUB_SHOT_TAB = '1'; HUB_SHOT_PAD = 'down,down,shot:at,a,shot:history' })
    Add-Shot "$layout-pop-verify" $layout '1280x720' (Merge $lib @{ HUB_SHOT_VERIFY = '1'; HUB_SHOT_DETAILS = 'fake:0' })
    Add-Shot "$layout-pop-hackrm" $layout '1280x720' (Merge $lib @{ HUB_SHOT_HACKS = '1'; HUB_SHOT_DETAILS = 'fake:0+h00000000'; HUB_SHOT_PAD = 'down,down,down,down,left,shot:at,a,shot:remove,b,shot:after_b' })
    Add-Shot "$layout-pop-versions" $layout '1280x720' (Merge $lib @{ HUB_SHOT_HACKS = '1'; HUB_SHOT_DETAILS = 'fake:0+h00000000'; HUB_SHOT_PAD = 'down,down,down,shot:at,a,shot:switched' })
    Add-Shot "$layout-pop-smash" $layout '1280x720' @{ HUB_SHOT_FAKE_LIB = 'native'; HUB_SHOT_DETAILS = 'native:smash64'; HUB_SHOT_PAD = 'right,right,a,down,shot:menu,a,shot:browser,back,shot:after_back,b,shot:after_b' }
    Add-Shot "$layout-pop-hsdropdown" $layout '1280x720' (Merge $oot @{ HUB_SHOT_SECS = '6'; HUB_SHOT_CLICK = $(if ($layout -eq 'trad') { '240,427' } else { '650,505' }); HUB_SHOT_PAD = 'wait,wait,wait,wait,wait,wait,wait,wait,wait,shot:list,b,shot:after_b' })
    Add-Shot "$layout-pop-hsbuilding" $layout '1280x720' @{ HUB_SHOT_FAKE_LIB = '1'; HUB_SHOT_HOOKSHOT = 'building'; HUB_SHOT_HOOKSHOT_AT = 'hookshot' }
}
# HUB_SHOT_ACT: every action with a QA name is pressed once (trace lines "act ...", "favorite ...", "savefolder ...", "controls ...")
foreach ($a in @('play', 'online', 'favorite', 'savefolder', 'cheats', 'controls', 'desktop')) {
    Add-Shot "trad-act-$a" 'trad' '1280x720' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_ACT = $a; HUB_BOOTANIM_FORCE = '1' })
    Add-Shot "desk-act-$a" 'desk' '1280x720' (Merge $lib @{ HUB_SHOT_DETAILS = 'fake:2'; HUB_SHOT_ACT = $a; HUB_BOOTANIM_FORCE = '1' })
}

$index = @()
foreach ($s in $shots) {
    if ($Only -and $s.n -notmatch $Only) { continue }
    if ($Set -eq 'base' -and -not $s.base) { continue }
    Wait-NoGame
    Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
    Remove-Item -Recurse -Force (Join-Path $sandbox 'userdata') -ErrorAction SilentlyContinue   # every shot starts from a fresh install
    $vault = Join-Path $sandbox 'vault'
    Remove-Item -Recurse -Force $vault -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force (Join-Path $vault '_account') | Out-Null
    $env:RETROHUB_QA = '1'; $env:SSB64_MUTE = '1'; $env:RETRORUN_MUTE = '1'; $env:N64RUN_MUTE = '1'
    $env:HUB_SHOT_VAULT = $vault
    $env:HUB_SHOT_SECS = '5'   # a shot may set its own
    $env:HUB_SHOT_SIZE = $s.size
    $env:HUB_SHOT_LAYOUT = if ($s.layout -eq 'desk') { 'desk' } else { 'traditional' }
    foreach ($k in $s.v.Keys) { Set-Item ("env:" + $k) $s.v[$k] }
    $name = if ($Tag -eq 'gp') { $s.n } else { "$($s.n)-$Tag" }
    $bmp = Join-Path $Out "$name.bmp"
    $env:HUB_SCREENSHOT = $bmp
    $env:HUB_SHOT_TRACE = Join-Path $Out "$name.trace"
    Get-ChildItem $Out -Filter "$name*.bmp" -ErrorAction SilentlyContinue | Remove-Item -Force
    Remove-Item -Force $env:HUB_SHOT_TRACE -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $exe -WorkingDirectory $sandbox -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Stop-Process -Id $p.Id -Force; Write-Host "${name}: timed out (ended PID $($p.Id))" }
    $p.WaitForExit()
    $code = $p.ExitCode
    foreach ($b in Get-ChildItem $Out -Filter "$name*.bmp") {   # the shot plus any shot:<name> frames of a pad walk
        if ($b.BaseName -ne $name -and -not $b.BaseName.StartsWith("$name-")) { continue }
        $img = [System.Drawing.Image]::FromFile($b.FullName)
        $img.Save([IO.Path]::ChangeExtension($b.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $img.Dispose()
    }
    $cap = if (Test-Path $env:HUB_SHOT_TRACE) { (Select-String -Path $env:HUB_SHOT_TRACE -Pattern '^captured ' | Select-Object -Last 1).Line } else { '' }
    $line = "$name | exit $code | $($s.layout) $($s.size) | " + (($s.v.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ') + " | $cap"
    Write-Host $line
    $index += $line
}
Get-ChildItem env: | Where-Object { $_.Name -like 'HUB_*' } | ForEach-Object { Remove-Item ("env:" + $_.Name) }
$index | Set-Content -Encoding utf8 (Join-Path $Out "index-$Tag.txt")
