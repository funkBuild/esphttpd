#!/bin/bash

# Robust QEMU test runner with forced exit on completion
# Monitors for test completion sentinel and forcefully exits

set -e

# Configuration
TIMEOUT=30
OUTPUT_FILE="qemu_test_output.txt"
SENTINEL_PASS="QEMU_TEST_COMPLETE: PASS"
SENTINEL_FAIL="QEMU_TEST_COMPLETE: FAIL"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}=== ESP HTTP Server QEMU Test Runner ===${NC}"

# Check ESP-IDF environment
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: IDF_PATH not set${NC}"
    echo "Please source ESP-IDF: . /path/to/esp-idf/export.sh"
    exit 1
fi

# Build the test app
echo -e "${YELLOW}Building test application...${NC}"
idf.py set-target esp32s3 2>/dev/null || true
idf.py build || {
    echo -e "${RED}Build failed!${NC}"
    exit 1
}

# Create QEMU flash image
echo -e "${YELLOW}Creating QEMU flash image...${NC}"
cd build

# Check if required files exist
if [ ! -f "bootloader/bootloader.bin" ] || [ ! -f "partition_table/partition-table.bin" ] || [ ! -f "esphttpd_test_app.bin" ]; then
    echo -e "${RED}Required binary files not found!${NC}"
    cd ..
    exit 1
fi

esptool.py --chip esp32s3 merge_bin \
    -o qemu_flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 esphttpd_test_app.bin > /dev/null 2>&1 || {
    echo -e "${RED}Failed to create flash image!${NC}"
    cd ..
    exit 1
}

cd ..

# Function to run QEMU and monitor output
run_qemu_with_monitoring() {
    local qemu_pid
    local monitor_pid
    local result=2  # Default to timeout

    # Start QEMU in background and save PID
    echo -e "${YELLOW}Starting QEMU ESP32S3 (timeout: ${TIMEOUT}s)...${NC}"
    echo "========================================="

    # Create a named pipe for output
    PIPE=$(mktemp -u)
    mkfifo "$PIPE"

    # Start QEMU with output to pipe
    qemu-system-xtensa \
        -machine esp32s3 \
        -drive file=build/qemu_flash.bin,format=raw,if=mtd \
        -serial pipe:$PIPE \
        -display none \
        -nographic 2>/dev/null &
    qemu_pid=$!

    # Monitor output in background
    (
        while IFS= read -r line; do
            echo "$line"
            echo "$line" >> "$OUTPUT_FILE"

            # Check for completion sentinels
            if [[ "$line" == *"$SENTINEL_PASS"* ]]; then
                echo "RESULT:PASS" > /tmp/qemu_result
                kill $qemu_pid 2>/dev/null || true
                break
            elif [[ "$line" == *"$SENTINEL_FAIL"* ]]; then
                echo "RESULT:FAIL" > /tmp/qemu_result
                kill $qemu_pid 2>/dev/null || true
                break
            fi
        done < "$PIPE"
    ) &
    monitor_pid=$!

    # Clear output file
    > "$OUTPUT_FILE"

    # Wait for timeout or completion
    local elapsed=0
    while [ $elapsed -lt $TIMEOUT ]; do
        # Check if QEMU is still running
        if ! kill -0 $qemu_pid 2>/dev/null; then
            break
        fi

        # Check if result file exists (test completed)
        if [ -f /tmp/qemu_result ]; then
            break
        fi

        sleep 1
        elapsed=$((elapsed + 1))
    done

    # Force kill QEMU if still running
    if kill -0 $qemu_pid 2>/dev/null; then
        echo -e "\n${YELLOW}Timeout reached, terminating QEMU...${NC}"
        kill -9 $qemu_pid 2>/dev/null || true
    fi

    # Kill monitor process
    kill $monitor_pid 2>/dev/null || true

    # Clean up pipe
    rm -f "$PIPE"

    # Check result
    if [ -f /tmp/qemu_result ]; then
        local test_result=$(cat /tmp/qemu_result)
        rm -f /tmp/qemu_result

        if [ "$test_result" = "RESULT:PASS" ]; then
            result=0
        else
            result=1
        fi
    fi

    return $result
}

# Run the tests
run_qemu_with_monitoring
TEST_RESULT=$?

echo "========================================="

# Parse and display results
case $TEST_RESULT in
    0)
        echo -e "${GREEN}✓ All tests passed!${NC}"

        # Count tests from output
        if grep -q "Tests run:" "$OUTPUT_FILE" 2>/dev/null; then
            grep "Tests run:" "$OUTPUT_FILE" | tail -1
        fi
        exit 0
        ;;
    1)
        echo -e "${RED}✗ Some tests failed!${NC}"

        # Show failures
        echo -e "${RED}Failed tests:${NC}"
        grep "FAIL:" "$OUTPUT_FILE" 2>/dev/null | grep -v "QEMU_TEST_COMPLETE" || echo "  Check $OUTPUT_FILE for details"
        exit 1
        ;;
    *)
        echo -e "${YELLOW}⚠ Tests did not complete within ${TIMEOUT} seconds${NC}"
        echo "Output saved to: $OUTPUT_FILE"

        # Show last few lines of output
        if [ -f "$OUTPUT_FILE" ]; then
            echo -e "\nLast output:"
            tail -5 "$OUTPUT_FILE"
        fi
        exit 2
        ;;
esac