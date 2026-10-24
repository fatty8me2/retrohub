# Dot-source from qa scripts:  . "$PSScriptRoot\_guard.ps1"; Assert-LocalServer $Server
# Test runs create throwaway accounts; the production server auto-friends every new account to the owner,
# so QA must use a local worker (npx wrangler dev --ip 127.0.0.1 --port 8761 --persist-to <fresh dir>).
$script:DefaultTestServer = 'ws://127.0.0.1:8761'
function Assert-LocalServer([string]$Server) {
    if ($Server -match 'smash-netplay\.jumpcircuit\.workers\.dev' -and $env:RETROHUB_ALLOW_PROD_TESTS -ne '1') {
        Write-Error "Refusing to run QA against the PRODUCTION server ($Server): it would create throwaway accounts auto-friended to the owner. Use a local worker, or set RETROHUB_ALLOW_PROD_TESTS=1 if you really mean it."
        exit 2
    }
}
