# Wrapper: runs scripts/run-lint.sh inside WSL Ubuntu from a Windows shell.
# Pass-through args; arbitrary flags like --fix or --warnings-as-errors work.
#
# Usage:
#     scripts\run-lint.ps1                       # warnings as warnings
#     scripts\run-lint.ps1 --warnings-as-errors  # CI mode
#     scripts\run-lint.ps1 --fix                 # apply safe auto-fixes

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $ProjectRoot
try {
    # Run wsl with console handles inherited so stderr flows directly to the
    # terminal as plain text. PowerShell 5.1's default native-command path
    # wraps every stderr line as an ErrorRecord, which mangles clang-tidy
    # output and breaks VS Code's problem matcher.
    $proc = Start-Process -FilePath wsl `
        -ArgumentList (@('bash', 'scripts/run-lint.sh') + $args) `
        -NoNewWindow -Wait -PassThru
    exit $proc.ExitCode
} finally {
    Pop-Location
}
