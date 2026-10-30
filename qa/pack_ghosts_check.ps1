# Read-only: runs the GamePack index against the REAL shared Drive packs (no extra links; community list from GitHub)
# and prints every console / display title the library would show as a greyed-out pack ghost. Downloads no game files.
param([string]$Exe = "$PSScriptRoot\..\build\bin\Release\qa_gamepack.exe", [string]$Links = "")
& $Exe index-list $Links
