# Runs every case of retrorun_rb_matrix.ps1 and prints a one-line summary each. Usage: retrorun_rb_all.ps1 -BinDir <dir> -Rom <rom> -Tag <tag> [-Extra "--rb-pred 8"]
param([string]$BinDir, [string]$Rom, [string]$Tag, [string]$Extra = '', [int]$Frames = 1800, [string[]]$Cases = @('direct', 'relay', 'bad60', 'bad120', 'asym'))
foreach ($c in $Cases) {
    $o = & (Join-Path $PSScriptRoot 'retrorun_rb_matrix.ps1') -BinDir $BinDir -Rom $Rom -Case $c -Tag $Tag -Extra $Extra -Frames $Frames
    $o | ForEach-Object { Write-Output $_ }
    Write-Output '----'
}
