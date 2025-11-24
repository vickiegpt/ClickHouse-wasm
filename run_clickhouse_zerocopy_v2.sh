#!/bin/bash
# Script to run ClickHouse with the io_uring zero-copy optimization library (v2)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEROCOPY_LIB="${SCRIPT_DIR}/libclickhouse_zerocopy_v2.so"

# Check if library exists
if [ ! -f "$ZEROCOPY_LIB" ]; then
    echo "Error: libclickhouse_zerocopy_v2.so not found in $SCRIPT_DIR"
    echo "Please build it first with: make -f Makefile.v2"
    exit 1
fi

# Check if liburing is available
if ! ldconfig -p | grep -q liburing; then
    echo "Warning: liburing not found in system libraries"
    echo "Please install liburing: apt-get install liburing2 (Debian/Ubuntu)"
    exit 1
fi

# Check if io_uring is enabled
if [ -f /proc/sys/kernel/io_uring_disabled ]; then
    DISABLED=$(cat /proc/sys/kernel/io_uring_disabled)
    if [ "$DISABLED" != "0" ]; then
        echo "Error: io_uring is disabled in the kernel"
        echo "Enable with: sudo sysctl kernel.io_uring_disabled=0"
        exit 1
    fi
fi

# Check kernel version
KERNEL_VERSION=$(uname -r | cut -d. -f1)
KERNEL_MINOR=$(uname -r | cut -d. -f2)
if [ "$KERNEL_VERSION" -lt 5 ] || ([ "$KERNEL_VERSION" -eq 5 ] && [ "$KERNEL_MINOR" -lt 1 ]); then
    echo "Warning: Kernel version $(uname -r) may not fully support io_uring"
    echo "Recommended: 5.19+"
fi

# Check if clickhouse-server is in PATH
if ! command -v clickhouse-server &> /dev/null; then
    echo "Warning: clickhouse-server not found in PATH"
    echo "You may need to specify the full path or install ClickHouse"
fi

# Configuration from environment or defaults
ZEROCOPY_DEBUG="${ZEROCOPY_DEBUG:-0}"
ZEROCOPY_THRESHOLD="${ZEROCOPY_THRESHOLD:-8192}"
ZEROCOPY_QUEUE_DEPTH="${ZEROCOPY_QUEUE_DEPTH:-128}"
ZEROCOPY_ASYNC="${ZEROCOPY_ASYNC:-0}"

# Show configuration
echo "================================================"
echo "ClickHouse Zero-Copy Optimization v2 (io_uring)"
echo "================================================"
echo "Library:      $ZEROCOPY_LIB"
echo "Debug:        $ZEROCOPY_DEBUG"
echo "Threshold:    $ZEROCOPY_THRESHOLD bytes"
echo "Queue depth:  $ZEROCOPY_QUEUE_DEPTH"
echo "Async mode:   $ZEROCOPY_ASYNC"
echo "Kernel:       $(uname -r)"
echo "================================================"
echo

# Export environment variables
export LD_PRELOAD="$ZEROCOPY_LIB"
export ZEROCOPY_DEBUG
export ZEROCOPY_THRESHOLD
export ZEROCOPY_QUEUE_DEPTH
export ZEROCOPY_ASYNC

# Run ClickHouse with remaining arguments
echo "Starting ClickHouse with io_uring zero-copy optimization..."
exec "$@"
