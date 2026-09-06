#!/usr/bin/env bash
# Runs the whole benchmark suite and prints the summary table.
#
#   ./benchmark.sh                 # default settings
#   ./benchmark.sh --timeout 300   # extra flags are forwarded to the solver
set -eo pipefail  # not -u: "$@" with no arguments trips older bash
cd "$(dirname "$0")"

BIN=${TCS_BIN:-}
if [ -z "$BIN" ]; then
    for candidate in build/turbocryptosat build/Release/turbocryptosat.exe build/turbocryptosat.exe; do
        if [ -x "$candidate" ]; then BIN="$candidate"; break; fi
    done
fi
if [ -z "$BIN" ]; then
    echo "solver binary not found; run ./build.sh first" >&2
    exit 1
fi

if [ -z "$(ls benchmark/*.cnf 2>/dev/null)" ]; then
    "$BIN" gen-benchmark benchmark
fi

exec "$BIN" benchmark benchmark --no-ui --timeout "${TCS_TIMEOUT:-120}" "$@"
