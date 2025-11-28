#!/bin/bash

# Script to build and run esphttpd tests on QEMU ESP32S3
# Requires: ESP-IDF with QEMU support

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}ESP HTTP Server Unity Tests - QEMU Runner${NC}"
echo "========================================="

# Check if IDF_PATH is set
if [ -z "$IDF_PATH" ]; then
    echo -e "${RED}Error: IDF_PATH not set. Please source ESP-IDF environment.${NC}"
    echo "Run: . /path/to/esp-idf/export.sh"
    exit 1
fi

# Check ESP-IDF version supports QEMU
if ! idf.py --version | grep -q "v5"; then
    echo -e "${YELLOW}Warning: ESP-IDF v5.x recommended for QEMU support${NC}"
fi

# Set target to ESP32S3
echo -e "${GREEN}Setting target to ESP32S3...${NC}"
idf.py set-target esp32s3

# Clean build directory
echo -e "${GREEN}Cleaning build directory...${NC}"
rm -rf build

# Build the test application
echo -e "${GREEN}Building test application...${NC}"
idf.py build

# Check if QEMU is available
if ! command -v qemu-system-xtensa &> /dev/null; then
    echo -e "${RED}Error: QEMU not found. Installing QEMU for ESP32...${NC}"
    echo "Follow instructions at: https://github.com/espressif/qemu/wiki"
    exit 1
fi

# Create QEMU flash image
echo -e "${GREEN}Creating QEMU flash image...${NC}"
cd build

# Merge binaries into single flash image
esptool.py --chip esp32s3 merge_bin \
    -o qemu_flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 esphttpd_test_app.bin

cd ..

# Run QEMU with timeout
echo -e "${GREEN}Starting QEMU ESP32S3...${NC}"
echo "========================================="

# Create QEMU command
QEMU_CMD="qemu-system-xtensa \
    -machine esp32s3 \
    -drive file=build/qemu_flash.bin,format=raw,if=mtd \
    -serial stdio \
    -display none"

# Run QEMU with timeout and capture output
TIMEOUT=30
OUTPUT_FILE="qemu_output.txt"

echo -e "${YELLOW}Running tests (timeout: ${TIMEOUT}s)...${NC}"
timeout $TIMEOUT $QEMU_CMD 2>&1 | tee $OUTPUT_FILE

# Check test results
if grep -q "QEMU_TEST_COMPLETE: PASS" $OUTPUT_FILE; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
elif grep -q "QEMU_TEST_COMPLETE: FAIL" $OUTPUT_FILE; then
    echo -e "${RED}✗ Some tests failed!${NC}"

    # Show failure summary
    echo -e "${RED}Failed tests:${NC}"
    grep "FAIL" $OUTPUT_FILE | grep -v "QEMU_TEST_COMPLETE"
    exit 1
else
    echo -e "${YELLOW}⚠ Tests did not complete within timeout${NC}"
    echo "Check $OUTPUT_FILE for details"
    exit 2
fi