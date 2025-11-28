#!/bin/bash

# E2E Test Server QEMU Runner with Port Forwarding
#
# This script builds and runs the E2E test server in QEMU with port forwarding
# The server will be accessible at http://localhost:8080 from the host

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}==================== E2E Test Server ====================${NC}"

# Build the application
echo -e "${YELLOW}Building E2E test application...${NC}"
if [ ! -f "CMakeLists.txt" ]; then
    echo -e "${RED}Error: CMakeLists.txt not found${NC}"
    exit 1
fi

# Clean build if requested
if [ "$1" = "clean" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf build
fi

# Build
idf.py build

if [ ! -f "build/esphttpd_e2e_test.elf" ]; then
    echo -e "${RED}Error: Build failed - ELF file not found${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful!${NC}"

# Create merged binary
echo -e "${YELLOW}Creating merged binary...${NC}"
esptool.py --chip esp32s3 merge_bin \
    -o build/merged.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/esphttpd_e2e_test.bin

# Create 4MB flash image (QEMU requires exactly 2, 4, 8, or 16MB)
dd if=/dev/zero of=build/flash.bin bs=1M count=4 2>/dev/null
dd if=build/merged.bin of=build/flash.bin conv=notrunc 2>/dev/null

echo -e "${GREEN}Flash image created${NC}"

# Kill any existing QEMU instance on our port
echo -e "${YELLOW}Checking for existing QEMU instances...${NC}"
if lsof -Pi :8080 -sTCP:LISTEN -t >/dev/null 2>&1 ; then
    echo -e "${YELLOW}Port 8080 is in use, attempting to free it...${NC}"
    pkill -f "qemu-system-xtensa.*hostfwd=tcp::8080" || true
    sleep 2
fi

# Run QEMU with port forwarding
echo -e "${CYAN}Starting QEMU with port forwarding (host:8080 -> guest:80)${NC}"
echo -e "${GREEN}Server will be available at: http://localhost:8080${NC}"
echo -e "${YELLOW}Press Ctrl+A then X to exit QEMU${NC}"
echo ""
echo -e "${CYAN}==================== QEMU Output ====================${NC}"

# Launch QEMU with:
# - OpenCores Ethernet MAC (open_eth)
# - Port forwarding from host 8080 to guest 80
# - Serial output to console
# Note: ESP32S3 in QEMU supports max 4MB RAM (default), don't use -m flag
qemu-system-xtensa \
    -nographic \
    -machine esp32s3 \
    -drive file=build/flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:8080-:80 \
    -serial mon:stdio \
    -no-reboot

echo ""
echo -e "${CYAN}QEMU terminated${NC}"