# Wrapper: runs scripts/run-tests.sh inside WSL Ubuntu from a Windows shell.
# Pass-through args (e.g. `scripts\run-tests.ps1 test:direct_io`).
#
# Usage:
#     scripts\run-tests.ps1                       # ceedling test:all
#     scripts\run-tests.ps1 test:direct_io        # just that test file
#     scripts\run-tests.ps1 clobber               # nuke build artifacts

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    # Inherit console handles so ceedling's output (and any error messages)
    # flow through as plain text, not wrapped as PowerShell ErrorRecords.
    $proc = Start-Process -FilePath wsl `
        -ArgumentList (@('bash', 'scripts/run-tests.sh') + $args) `
        -NoNewWindow -Wait -PassThru
    exit $proc.ExitCode
} finally {
    Pop-Location
}
