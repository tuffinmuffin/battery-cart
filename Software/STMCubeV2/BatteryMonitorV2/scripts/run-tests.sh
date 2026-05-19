#!/usr/bin/env bash
# Host-side unit tests via Ceedling (Unity + CMock).
#
# Prerequisites (one-time): `bash scripts/setup-wsl.sh` installs Ruby and the
# `ceedling` gem along with everything else.
#
# Forwards args to ceedling, so e.g. `bash scripts/run-tests.sh test:direct_io`
# runs just that file's tests.

set -euo pipefail

if ! command -v ceedling >/dev/null 2>&1; then
    echo "error: ceedling not on PATH" >&2
    echo "Run first: bash scripts/setup-wsl.sh" >&2
    exit 2
fi

cd test
if [ "$#" -eq 0 ]; then
    exec ceedling test:all
else
    exec ceedling "$@"
fi
