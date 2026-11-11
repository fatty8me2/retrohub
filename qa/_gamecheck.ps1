# Path-aware "is the owner gaming?" check. Dot-source it, then call Wait-NoGame (blocks, re-checks every 60 s) or Test-OwnerGaming.
# A matching process whose Path is under ...\SmashClassicPort\wt\ is an agent's headless test and is ignored.
function Get-OwnerGameProcs {
    Get-Process lichii,eden-cli,hookshot,Dolphin,Cemu,azahar,pcsx2-qt,PPSSPPWindows64,retrorun,n64run -ErrorAction SilentlyContinue |
        Where-Object { -not ($_.Path -and $_.Path -like '*\SmashClassicPort\wt\*') }
}
function Test-OwnerGaming { return [bool](@(Get-OwnerGameProcs).Count) }
function Wait-NoGame {
    while (Test-OwnerGaming) {
        Write-Host ("owner is gaming: " + ((Get-OwnerGameProcs | ForEach-Object { $_.Name }) -join ',') + "; waiting 60 s")
        Start-Sleep -Seconds 60
    }
}
