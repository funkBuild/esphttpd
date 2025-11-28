#!/bin/bash

# Simple and robust QEMU test runner using timeout command
# Automatically exits after completion or timeout

set -e

# Configuration
TIMEOUT_SEC=30
OUTPUT_FILE="test_output.txt"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}ESP HTTP Server QEMU Test Runner${NC}"
echo "=================================="

# Check environment
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF not sourced${NC}"
    echo "Run: . \$IDF_PATH/export.sh"
    exit 1
fi

# Function to check for qemu-system-xtensa
check_qemu() {
    if ! command -v qemu-system-xtensa &> /dev/null; then
        echo -e "${RED}Error: qemu-system-xtensa not found${NC}"
        echo "Install QEMU for ESP32: https://github.com/espressif/qemu"
        exit 1
    fi
}

# Build test app
build_app() {
    echo -e "${YELLOW}Building test app for ESP32S3...${NC}"

    # Set target (ignore error if already set)
    idf.py set-target esp32s3 &>/dev/null || true

    # Build
    if ! idf.py build; then
        echo -e "${RED}Build failed!${NC}"
        exit 1
    fi
}

# Create flash image
create_flash_image() {
    echo -e "${YELLOW}Creating flash image...${NC}"

    cd build

    esptool.py --chip esp32s3 merge_bin \
        -o flash_image.bin \
        --flash_mode dio \
        --flash_size 4MB \
        --fill-flash-size 4MB \
        0x0 bootloader/bootloader.bin \
        0x8000 partition_table/partition-table.bin \
        0x10000 esphttpd_test_app.bin \
        &>/dev/null

    local result=$?
    cd ..
    return $result
}

# Run tests in QEMU
run_tests() {
    echo -e "${YELLOW}Running tests (timeout: ${TIMEOUT_SEC}s)...${NC}"
    echo "------------------------------------------"

    # Run QEMU with timeout, capture output
    # Use mon:stdio for better compatibility
    timeout --preserve-status ${TIMEOUT_SEC} \
        qemu-system-xtensa \
            -machine esp32s3 \
            -drive file=build/flash_image.bin,format=raw,if=mtd \
            -serial mon:stdio \
            -display none \
            2>&1 | tee "${OUTPUT_FILE}"

    # The exit code from timeout
    local exit_code=$?

    echo "------------------------------------------"

    # Check what happened
    if [ $exit_code -eq 124 ] || [ $exit_code -eq 137 ]; then
        # Timeout occurred
        echo -e "${YELLOW}⏱ Test timeout after ${TIMEOUT_SEC} seconds${NC}"

        # Check if tests actually completed but QEMU didn't exit
        if grep -q "QEMU_TEST_COMPLETE: PASS" "${OUTPUT_FILE}"; then
            echo -e "${GREEN}✓ Tests passed (QEMU didn't exit cleanly)${NC}"
            return 0
        elif grep -q "QEMU_TEST_COMPLETE: FAIL" "${OUTPUT_FILE}"; then
            echo -e "${RED}✗ Tests failed${NC}"
            return 1
        else
            echo -e "${YELLOW}Tests incomplete - check ${OUTPUT_FILE}${NC}"
            return 2
        fi
    else
        # QEMU exited normally or was killed by our sentinel
        if grep -q "QEMU_TEST_COMPLETE: PASS" "${OUTPUT_FILE}"; then
            echo -e "${GREEN}✓ All tests passed!${NC}"

            # Show test summary if available
            grep -E "Tests? [0-9]+ Failures?" "${OUTPUT_FILE}" | tail -1 || true
            return 0
        elif grep -q "QEMU_TEST_COMPLETE: FAIL" "${OUTPUT_FILE}"; then
            echo -e "${RED}✗ Tests failed!${NC}"

            # Show failures
            echo -e "${RED}Failures:${NC}"
            grep "FAIL:" "${OUTPUT_FILE}" | head -10 || true
            return 1
        else
            echo -e "${YELLOW}⚠ Unexpected exit${NC}"
            return 3
        fi
    fi
}

# Main execution
main() {
    local start_time=$(date +%s)

    # Step 1: Check QEMU
    check_qemu

    # Step 2: Build
    build_app

    # Step 3: Create flash image
    if ! create_flash_image; then
        echo -e "${RED}Failed to create flash image${NC}"
        exit 1
    fi

    # Step 4: Run tests
    run_tests
    local test_result=$?

    # Calculate elapsed time
    local end_time=$(date +%s)
    local elapsed=$((end_time - start_time))

    echo ""
    echo -e "${CYAN}Total time: ${elapsed} seconds${NC}"
    echo -e "${CYAN}Output saved to: ${OUTPUT_FILE}${NC}"

    exit $test_result
}

# Run if executed directly
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    main "$@"
fi