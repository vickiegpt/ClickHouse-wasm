#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEROCOPY_LIB="${SCRIPT_DIR}/libclickhouse_zerocopy_v2.so"
DEFAULT_JOB="${SCRIPT_DIR}/fio_zerocopy_v2.fio"

if [ ! -f "$ZEROCOPY_LIB" ]; then
    echo "Error: libclickhouse_zerocopy_v2.so not found in $SCRIPT_DIR"
    echo "Build it first with: make -f Makefile.v2"
    exit 1
fi

if ! command -v fio >/dev/null 2>&1; then
    echo "Error: fio is not installed"
    exit 1
fi

if [ $# -eq 0 ]; then
    set -- "$DEFAULT_JOB"
fi

export LD_PRELOAD="$ZEROCOPY_LIB"
export ZEROCOPY_THRESHOLD="${ZEROCOPY_THRESHOLD:-8192}"
export ZEROCOPY_QUEUE_DEPTH="${ZEROCOPY_QUEUE_DEPTH:-128}"
export ZEROCOPY_DEBUG="${ZEROCOPY_DEBUG:-0}"

echo "========================================"
echo "fio + zerocopy v2"
echo "========================================"
echo "Library:      $ZEROCOPY_LIB"
echo "Threshold:    $ZEROCOPY_THRESHOLD"
echo "Queue depth:  $ZEROCOPY_QUEUE_DEPTH"
echo "Debug:        $ZEROCOPY_DEBUG"
echo "Command:      fio $*"
echo "========================================"

exec fio "$@"
