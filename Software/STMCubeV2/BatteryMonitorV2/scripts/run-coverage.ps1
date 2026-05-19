# Wrapper: runs scripts/run-coverage.sh inside WSL Ubuntu from a Windows shell.

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    $proc = Start-Process -FilePath wsl `
        -ArgumentList (@('bash', 'scripts/run-coverage.sh') + $args) `
        -NoNewWindow -Wait -PassThru
    exit $proc.ExitCode
} finally {
    Pop-Location
}
