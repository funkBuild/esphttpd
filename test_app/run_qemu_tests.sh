#!/bin/bash

# Script to build and run esphttpd tests on QEMU ESP32S3
# Requires: ESP-IDF with QEMU support

set -e

# Make all relative paths deterministic regardless of the caller's cwd.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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

# Clean stale or incompatible build metadata before set-target. idf.py refuses
# to clean a directory that lacks a valid CMake cache.
echo -e "${GREEN}Cleaning build directory...${NC}"
rm -rf build

# Set target to ESP32S3
echo -e "${GREEN}Setting target to ESP32S3...${NC}"
idf.py set-target esp32s3

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

# Merge binaries into single flash image.
# --fill-flash-size pads the image to a QEMU-valid size (2/4/8/16MB);
# without it QEMU rejects the raw image ("Drive size error"). 2MB matches
# CONFIG_ESPTOOLPY_FLASHSIZE in sdkconfig.
esptool.py --chip esp32s3 merge_bin \
    -o qemu_flash.bin \
    --flash_mode dio \
    --flash_size 2MB \
    --fill-flash-size 2MB \
    0x0 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 esphttpd_test_app.bin

cd ..

# The ESP32-S3 machine + efuse device are only in the Espressif QEMU build;
# the distro /usr/bin/qemu-system-xtensa lacks them. Prefer the Espressif one.
find_qemu() {
    local QEMU_DIR="$HOME/.espressif/tools/qemu-xtensa"
    if [ -d "$QEMU_DIR" ]; then
        local found
        found=$(find "$QEMU_DIR" -name "qemu-system-xtensa" -type f 2>/dev/null | sort -rV | head -1)
        if [ -n "$found" ] && [ -x "$found" ]; then
            echo "$found"; return
        fi
    fi
    which qemu-system-xtensa 2>/dev/null || true
}
QEMU_BIN=$(find_qemu)

# The efuse device needs a backing image or the ESP32-S3 machine won't boot.
if [ ! -f build/qemu_efuse.bin ]; then
    dd if=/dev/zero of=build/qemu_efuse.bin bs=1K count=4 2>/dev/null
fi

# Run QEMU with timeout
echo -e "${GREEN}Starting QEMU ESP32S3...${NC}"
echo "========================================="

# Run QEMU with a ceiling timeout; kill early when the completion sentinel
# appears (QEMU does not exit on its own). QEMU serial output contains NUL
# bytes from early ROM boot, so every grep over the log must use -a (text).
TIMEOUT=300
OUTPUT_FILE="qemu_output.txt"

echo -e "${YELLOW}Running tests (timeout: ${TIMEOUT}s)...${NC}"

"$QEMU_BIN" \
    -M esp32s3 \
    -drive file=build/qemu_flash.bin,if=mtd,format=raw \
    -drive file=build/qemu_efuse.bin,if=none,format=raw,id=efuse \
    -serial mon:stdio \
    -nographic \
    -no-reboot \
    > "$OUTPUT_FILE" 2>&1 &
QEMU_PID=$!

SECONDS_WAITED=0
while [ $SECONDS_WAITED -lt $TIMEOUT ]; do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    if grep -a -q "QEMU_TEST_COMPLETE:" "$OUTPUT_FILE" 2>/dev/null; then
        sleep 1  # allow trailing output to flush
        break
    fi
    sleep 2
    SECONDS_WAITED=$((SECONDS_WAITED + 2))
done

# Force-kill QEMU (it does not exit on its own)
if kill -0 "$QEMU_PID" 2>/dev/null; then
    kill -9 "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi

# Show the Unity summary line if present
grep -a -E "Tests [0-9]+ Failures [0-9]+ Ignored" "$OUTPUT_FILE" | tail -1 || true

# Check test results
if grep -a -q "QEMU_TEST_COMPLETE: PASS" "$OUTPUT_FILE"; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
elif grep -a -q "QEMU_TEST_COMPLETE: FAIL" "$OUTPUT_FILE"; then
    echo -e "${RED}✗ Some tests failed!${NC}"

    # Show failure summary
    echo -e "${RED}Failed tests:${NC}"
    grep -a ":FAIL" "$OUTPUT_FILE" | grep -v "QEMU_TEST_COMPLETE"
    exit 1
else
    echo -e "${YELLOW}⚠ Tests did not complete within timeout${NC}"
    echo "Check $OUTPUT_FILE for details"
    exit 2
fi
