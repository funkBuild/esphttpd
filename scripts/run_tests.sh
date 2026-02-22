#!/bin/bash
#
# esphttpd Unit Test Runner
# Builds, runs tests under QEMU, and reports results
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
TEST_DIR="$PROJECT_DIR/test_app"
BUILD_DIR="$TEST_DIR/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Log file with timestamp
LOG_FILE="/tmp/esphttpd_unit_tests_$(date +%Y%m%d_%H%M%S).log"

# Find ESP-IDF
find_esp_idf() {
    local idf_paths=(
        "$IDF_PATH"
        "$HOME/esp/esp-idf"
        "/opt/esp-idf"
    )

    for path in "${idf_paths[@]}"; do
        if [ -f "$path/export.sh" ]; then
            echo "$path"
            return 0
        fi
    done

    echo ""
    return 1
}

# Find QEMU binary
find_qemu() {
    # Check PATH first
    if command -v qemu-system-xtensa &>/dev/null; then
        which qemu-system-xtensa
        return 0
    fi

    # Search common locations
    local qemu_paths=(
        "$HOME/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa"
        "$HOME/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa"
    )

    for pattern in "${qemu_paths[@]}"; do
        for path in $pattern; do
            if [ -x "$path" ]; then
                echo "$path"
                return 0
            fi
        done
    done

    echo ""
    return 1
}

# Cleanup function
cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "Terminating QEMU (PID: $QEMU_PID)..."
        kill -9 "$QEMU_PID" 2>/dev/null || true
    fi
    # Kill any stray QEMU processes
    pkill -9 qemu-system-xtensa 2>/dev/null || true
}
trap cleanup EXIT

# Print header
print_header() {
    echo ""
    echo "============================================================"
    echo "$1"
    echo "============================================================"
}

# Main execution
main() {
    print_header "esphttpd Unit Test Runner"
    echo "Log file: $LOG_FILE"
    echo ""

    # Find and source ESP-IDF
    local idf_path=$(find_esp_idf)
    if [ -z "$idf_path" ]; then
        echo -e "${RED}ERROR: ESP-IDF not found${NC}"
        exit 1
    fi
    echo "Using ESP-IDF: $idf_path"
    source "$idf_path/export.sh" >/dev/null 2>&1

    # Find QEMU
    local qemu_bin=$(find_qemu)
    if [ -z "$qemu_bin" ]; then
        echo -e "${RED}ERROR: qemu-system-xtensa not found${NC}"
        exit 1
    fi
    echo "Using QEMU: $qemu_bin"

    # Kill any existing QEMU processes
    echo "Killing any existing QEMU processes..."
    pkill -9 qemu-system-xtensa 2>/dev/null || true
    sleep 1

    # Build
    print_header "Building Test Application"
    cd "$TEST_DIR"
    if ! idf.py build 2>&1 | tee -a "$LOG_FILE"; then
        echo -e "${RED}Build failed${NC}"
        echo ""
        echo "Log file: $LOG_FILE"
        exit 1
    fi
    echo -e "${GREEN}Build successful${NC}"

    # Create QEMU flash image
    print_header "Creating QEMU Flash Image"
    cd "$BUILD_DIR"
    esptool.py --chip=esp32s3 merge_bin --output=qemu_flash.bin \
        --fill-flash-size=2MB --flash_mode dio --flash_freq 80m --flash_size 2MB \
        0x0 bootloader/bootloader.bin \
        0x10000 esphttpd_test_app.bin \
        0x8000 partition_table/partition-table.bin 2>&1 | tee -a "$LOG_FILE"

    # Create efuse file
    [ ! -f qemu_efuse.bin ] && dd if=/dev/zero of=qemu_efuse.bin bs=1K count=4 2>/dev/null

    # Run QEMU
    print_header "Running Tests Under QEMU"

    # Create a named pipe for QEMU output
    local fifo="/tmp/qemu_output_$$"
    mkfifo "$fifo"

    # Start QEMU in background, writing to both the pipe and log file
    "$qemu_bin" -M esp32s3 \
        -drive file=qemu_flash.bin,if=mtd,format=raw \
        -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
        -global driver=nvram.esp32c3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -nic user,model=open_eth \
        -nographic 2>&1 | tee "$fifo" >> "$LOG_FILE" &
    QEMU_PID=$!

    echo "QEMU started (PID: $QEMU_PID)"
    echo "Waiting for tests to complete..."
    echo ""

    # Monitor output for completion marker
    local test_result=""
    local timeout_seconds=180
    local start_time=$(date +%s)

    while IFS= read -r line; do
        echo "$line"

        if [[ "$line" == *"QEMU_TEST_COMPLETE: PASS"* ]]; then
            test_result="PASS"
            break
        elif [[ "$line" == *"QEMU_TEST_COMPLETE: FAIL"* ]]; then
            test_result="FAIL"
            break
        fi

        # Check timeout
        local current_time=$(date +%s)
        if (( current_time - start_time > timeout_seconds )); then
            echo -e "${RED}ERROR: Test timeout after ${timeout_seconds}s${NC}"
            test_result="TIMEOUT"
            break
        fi
    done < "$fifo"

    # Cleanup pipe
    rm -f "$fifo"

    # Terminate QEMU
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        echo ""
        echo "Terminating QEMU..."
        kill -9 "$QEMU_PID" 2>/dev/null || true
    fi
    QEMU_PID=""

    # Print results
    print_header "Test Results"

    # Extract Unity summary from log
    local passed=$(grep -c ":PASS" "$LOG_FILE" 2>/dev/null || echo "0")
    local failed=$(grep -c ":FAIL" "$LOG_FILE" 2>/dev/null || echo "0")
    local ignored=$(grep -c ":IGNORE" "$LOG_FILE" 2>/dev/null || echo "0")

    echo "Passed:  $passed"
    echo "Failed:  $failed"
    echo "Ignored: $ignored"
    echo ""

    if [ "$test_result" = "PASS" ]; then
        echo -e "${GREEN}ALL TESTS PASSED${NC}"
        echo ""
        echo "Log file: $LOG_FILE"
        exit 0
    elif [ "$test_result" = "FAIL" ]; then
        echo -e "${RED}TESTS FAILED${NC}"
        echo ""
        echo "Failed tests:"
        grep ":FAIL" "$LOG_FILE" 2>/dev/null || true
        echo ""
        echo "Log file: $LOG_FILE"
        exit 1
    else
        echo -e "${RED}TESTS DID NOT COMPLETE (timeout or error)${NC}"
        echo ""
        echo "Log file: $LOG_FILE"
        exit 1
    fi
}

main "$@"
