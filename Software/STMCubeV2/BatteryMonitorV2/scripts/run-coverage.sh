#!/usr/bin/env bash
# Host-side unit tests with coverage instrumentation via Ceedling + gcovr.
#
# Same setup-wsl.sh prereq as run-tests.sh. Adds an HTML report (open in a
# browser) and a Cobertura XML report (the format Codecov / GitHub PR
# overlays consume).
#
# Outputs land under test/build/artifacts/gcov/gcovr/. HTML report at:
#     test/build/artifacts/gcov/gcovr/GcovCoverageResults.html

set -euo pipefail

if ! command -v ceedling >/dev/null 2>&1; then
    echo "error: ceedling not on PATH (run: bash scripts/setup-wsl.sh)" >&2
    exit 2
fi
if ! command -v gcovr >/dev/null 2>&1; then
    echo "error: gcovr not on PATH (run: bash scripts/setup-wsl.sh)" >&2
    exit 2
fi

cd test
ceedling gcov:all

# Surface the report locations after the run
ARTIFACTS_DIR=test/build/artifacts/gcov/gcovr
if [ -d "$ARTIFACTS_DIR" ]; then
    echo ""
    echo "Coverage artifacts:"
    ls -1 "$ARTIFACTS_DIR" | sed 's|^|  '"$ARTIFACTS_DIR"'/|'
fi
