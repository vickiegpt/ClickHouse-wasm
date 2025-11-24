#!/bin/bash
# Script to run ClickHouse with the zero-copy optimization library

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEROCOPY_LIB="${SCRIPT_DIR}/libclickhouse_zerocopy.so"

# Check if library exists
if [ ! -f "$ZEROCOPY_LIB" ]; then
    echo "Error: libclickhouse_zerocopy.so not found in $SCRIPT_DIR"
    echo "Please build it first with: make"
    exit 1
fi

# Check if clickhouse-server is in PATH
if ! command -v clickhouse-server &> /dev/null; then
    echo "Warning: clickhouse-server not found in PATH"
    echo "You may need to specify the full path or install ClickHouse"
fi

# Configuration from environment or defaults
ZEROCOPY_DEBUG="${ZEROCOPY_DEBUG:-0}"
ZEROCOPY_THRESHOLD="${ZEROCOPY_THRESHOLD:-4096}"

# Show configuration
echo "=================================="
echo "ClickHouse Zero-Copy Optimization"
echo "=================================="
echo "Library:    $ZEROCOPY_LIB"
echo "Debug:      $ZEROCOPY_DEBUG"
echo "Threshold:  $ZEROCOPY_THRESHOLD bytes"
echo "=================================="
echo

# Export environment variables
export LD_PRELOAD="$ZEROCOPY_LIB"
export ZEROCOPY_DEBUG
export ZEROCOPY_THRESHOLD

# Run ClickHouse with remaining arguments
echo "Starting ClickHouse with zero-copy optimization..."
exec "$@"
