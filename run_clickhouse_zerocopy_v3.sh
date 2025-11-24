#!/bin/bash
# Script to run ClickHouse with BPF bypass zero-copy optimization library (v3)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZEROCOPY_LIB="${SCRIPT_DIR}/libclickhouse_zerocopy_v3.so"

echo "=================================================="
echo "ClickHouse Zero-Copy Optimization v3"
echo "BPF Bypass + DMA Edition"
echo "=================================================="
echo

# Check if library exists
if [ ! -f "$ZEROCOPY_LIB" ]; then
    echo "Error: libclickhouse_zerocopy_v3.so not found in $SCRIPT_DIR"
    echo "Please build it first with: make -f Makefile.v3"
    exit 1
fi

# Check if running as root (required for DMA)
if [ "$EUID" -ne 0 ]; then
    echo "⚠ Warning: Not running as root"
    echo "  DMA buffer allocation requires root privileges"
    echo "  Please run with: sudo $0 $@"
    echo
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Check dependencies
if ! ldconfig -p | grep -q liburing; then
    echo "Error: liburing not found"
    echo "Install with: apt-get install liburing2 (Debian/Ubuntu)"
    exit 1
fi

# Check kernel version
KERNEL_VERSION=$(uname -r | cut -d. -f1)
KERNEL_MINOR=$(uname -r | cut -d. -f2)
if [ "$KERNEL_VERSION" -lt 5 ] || ([ "$KERNEL_VERSION" -eq 5 ] && [ "$KERNEL_MINOR" -lt 10 ]); then
    echo "⚠ Warning: Kernel $(uname -r) may not fully support all features"
    echo "  Recommended: 5.19+"
fi

# Check hugepages
HUGEPAGES_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
if [ "$HUGEPAGES_FREE" -lt 16 ]; then
    echo "⚠ Warning: Low hugepages available ($HUGEPAGES_FREE)"
    echo "  Recommend: sudo sysctl -w vm.nr_hugepages=512"
fi

# Check io_uring
if [ -f /proc/sys/kernel/io_uring_disabled ]; then
    DISABLED=$(cat /proc/sys/kernel/io_uring_disabled)
    if [ "$DISABLED" != "0" ]; then
        echo "⚠ Warning: io_uring is disabled"
        echo "  Enable with: sudo sysctl -w kernel.io_uring_disabled=0"
    fi
fi

# Check memory lock limits
MEMLOCK_LIMIT=$(ulimit -l)
if [ "$MEMLOCK_LIMIT" != "unlimited" ] && [ "$MEMLOCK_LIMIT" -lt 1048576 ]; then
    echo "⚠ Warning: Low memory lock limit ($MEMLOCK_LIMIT KB)"
    echo "  Recommend: ulimit -l unlimited"
fi

# Configuration from environment or defaults
ZEROCOPY_DEBUG="${ZEROCOPY_DEBUG:-0}"
ZEROCOPY_THRESHOLD="${ZEROCOPY_THRESHOLD:-16384}"
ZEROCOPY_BYPASS_MODE="${ZEROCOPY_BYPASS_MODE:-full}"
ZEROCOPY_DIRECT_IO="${ZEROCOPY_DIRECT_IO:-1}"
ZEROCOPY_NIC_QUEUE="${ZEROCOPY_NIC_QUEUE:-4}"

# Show configuration
echo "Configuration:"
echo "  Library:       $ZEROCOPY_LIB"
echo "  Debug:         $ZEROCOPY_DEBUG"
echo "  Threshold:     $ZEROCOPY_THRESHOLD bytes"
echo "  Bypass mode:   $ZEROCOPY_BYPASS_MODE"
echo "  Direct I/O:    $ZEROCOPY_DIRECT_IO"
echo "  NIC queues:    $ZEROCOPY_NIC_QUEUE"
echo "  Kernel:        $(uname -r)"
echo "  Hugepages:     $HUGEPAGES_FREE free"
echo "  User:          $(whoami)"
echo "=================================================="
echo

# System requirements check
echo "System Requirements:"
make -f "$SCRIPT_DIR/Makefile.v3" check 2>/dev/null | grep -E "✓|✗|⚠" | head -10
echo

# Offer to setup system
if [ "$EUID" -eq 0 ]; then
    echo "Would you like to optimize system settings? (recommended for first run)"
    read -p "Run setup? (y/N) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        make -f "$SCRIPT_DIR/Makefile.v3" setup
        echo
    fi
fi

# Export environment variables
export LD_PRELOAD="$ZEROCOPY_LIB"
export ZEROCOPY_DEBUG
export ZEROCOPY_THRESHOLD
export ZEROCOPY_BYPASS_MODE
export ZEROCOPY_DIRECT_IO
export ZEROCOPY_NIC_QUEUE

# Run ClickHouse
echo "Starting ClickHouse with BPF bypass + DMA optimization..."
echo "Press Ctrl+C to stop and show statistics"
echo

# Trap to show stats on exit
trap 'echo; echo "Showing statistics..."; sleep 1' EXIT

exec "$@"
