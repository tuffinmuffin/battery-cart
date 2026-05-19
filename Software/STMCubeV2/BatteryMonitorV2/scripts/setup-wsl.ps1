# Wrapper: runs scripts/setup-wsl.sh inside WSL Ubuntu from a Windows shell.
# Handles the path/quoting between PowerShell -> wsl -> bash so callers don't have to.
#
# Usage (from anywhere):
#     powershell -ExecutionPolicy Bypass -File scripts\setup-wsl.ps1
# Or just:
#     scripts\setup-wsl.ps1     (if execution policy permits)

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    # Run with console handles inherited so sudo prompts and apt progress
    # flow through normally (not wrapped as PowerShell ErrorRecords).
    $proc = Start-Process -FilePath wsl `
        -ArgumentList (@('bash', 'scripts/setup-wsl.sh') + $args) `
        -NoNewWindow -Wait -PassThru
    exit $proc.ExitCode
} finally {
    Pop-Location
}
