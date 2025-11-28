#!/bin/bash

# E2E Test Runner for esphttpd
# Starts QEMU ESP32S3 emulator, runs Jest tests, cleans up

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Configuration
PORT=8088
SERVER_URL="http://127.0.0.1:${PORT}"
MAX_WAIT_SECONDS=60
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Log files
LOG_DIR="${SCRIPT_DIR}/logs"
QEMU_LOG="${LOG_DIR}/qemu_${TIMESTAMP}.log"
JEST_LOG="${LOG_DIR}/jest_${TIMESTAMP}.log"
BUILD_LOG="${LOG_DIR}/build_${TIMESTAMP}.log"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Track QEMU process
QEMU_PID=""
EXIT_CODE=0

#==============================================================================
# Helper Functions
#==============================================================================

log_info()  { echo -e "${CYAN}[INFO]${NC} $1"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()  { echo -e "\n${BOLD}${CYAN}==> $1${NC}"; }

cleanup() {
    log_step "Cleaning up"

    # Kill our QEMU process
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        log_info "Stopping QEMU (PID: $QEMU_PID)"
        kill -9 "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi

    # Kill any remaining QEMU processes
    pkill -9 qemu-system-xtensa 2>/dev/null || true

    # Wait for port to be released
    sleep 1

    log_ok "Cleanup complete"
}

kill_existing_qemu() {
    log_info "Checking for existing QEMU processes..."

    # Kill any QEMU processes
    if pgrep -x qemu-system-xtensa > /dev/null 2>&1; then
        log_warn "Found existing QEMU processes, killing them..."
        pkill -9 qemu-system-xtensa 2>/dev/null || true
        sleep 2
    fi

    # Check if port is in use
    if lsof -i :${PORT} -sTCP:LISTEN -t >/dev/null 2>&1; then
        log_warn "Port ${PORT} is in use, killing process..."
        kill -9 $(lsof -i :${PORT} -sTCP:LISTEN -t) 2>/dev/null || true
        sleep 2
    fi

    # Verify port is free
    if lsof -i :${PORT} -sTCP:LISTEN -t >/dev/null 2>&1; then
        log_error "Failed to free port ${PORT}"
        exit 1
    fi

    log_ok "No conflicting processes"
}

wait_for_server() {
    log_info "Waiting for server to be ready (max ${MAX_WAIT_SECONDS}s)..."

    local waited=0
    while [ $waited -lt $MAX_WAIT_SECONDS ]; do
        if curl -s --fail --max-time 2 "${SERVER_URL}/" >/dev/null 2>&1; then
            log_ok "Server is responding at ${SERVER_URL}"
            return 0
        fi

        # Check if QEMU is still running
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            log_error "QEMU process died unexpectedly"
            log_error "Check log: ${QEMU_LOG}"
            tail -30 "$QEMU_LOG" 2>/dev/null || true
            return 1
        fi

        sleep 1
        waited=$((waited + 1))
        printf "."
    done

    echo ""
    log_error "Server failed to start within ${MAX_WAIT_SECONDS} seconds"
    log_error "Check log: ${QEMU_LOG}"
    tail -50 "$QEMU_LOG" 2>/dev/null || true
    return 1
}

#==============================================================================
# Main Script
#==============================================================================

trap cleanup EXIT

echo -e "${BOLD}${CYAN}"
echo "============================================================"
echo "           ESP32 HTTP Server E2E Test Suite"
echo "============================================================"
echo -e "${NC}"

# Create log directory
mkdir -p "$LOG_DIR"

echo -e "${BOLD}Log files:${NC}"
echo "  Build: ${BUILD_LOG}"
echo "  QEMU:  ${QEMU_LOG}"
echo "  Jest:  ${JEST_LOG}"
echo ""

#------------------------------------------------------------------------------
# Step 1: Kill existing processes
#------------------------------------------------------------------------------
log_step "Step 1: Ensuring clean environment"
kill_existing_qemu

#------------------------------------------------------------------------------
# Step 2: Build the project
#------------------------------------------------------------------------------
log_step "Step 2: Building E2E test server"
log_info "Running idf.py build (output: ${BUILD_LOG})"

if ! idf.py build > "$BUILD_LOG" 2>&1; then
    log_error "Build failed! Check ${BUILD_LOG}"
    tail -30 "$BUILD_LOG"
    exit 1
fi

if [ ! -f "build/esphttpd_e2e_test.bin" ]; then
    log_error "Build output not found"
    exit 1
fi

log_ok "Build successful"

#------------------------------------------------------------------------------
# Step 3: Create flash image
#------------------------------------------------------------------------------
log_step "Step 3: Creating flash image"

# Merge binaries
esptool.py --chip esp32s3 merge_bin \
    -o build/merged.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/esphttpd_e2e_test.bin \
    >> "$BUILD_LOG" 2>&1

# Create 4MB padded flash image (QEMU requires exact sizes)
dd if=/dev/zero of=build/flash.bin bs=1M count=4 2>/dev/null
dd if=build/merged.bin of=build/flash.bin conv=notrunc 2>/dev/null

log_ok "Flash image created"

#------------------------------------------------------------------------------
# Step 4: Start QEMU
#------------------------------------------------------------------------------
log_step "Step 4: Starting QEMU emulator"
log_info "QEMU log: ${QEMU_LOG}"

qemu-system-xtensa \
    -nographic \
    -machine esp32s3 \
    -drive file=build/flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:${PORT}-:80 \
    -serial mon:stdio \
    -no-reboot \
    > "$QEMU_LOG" 2>&1 &

QEMU_PID=$!
log_info "QEMU started with PID: ${QEMU_PID}"

# Wait for server
if ! wait_for_server; then
    EXIT_CODE=1
    exit 1
fi

#------------------------------------------------------------------------------
# Step 5: Quick connectivity test
#------------------------------------------------------------------------------
log_step "Step 5: Verifying connectivity"

RESPONSE=$(curl -s "${SERVER_URL}/")
if echo "$RESPONSE" | grep -q "ESP32"; then
    log_ok "Server responding correctly"
    log_info "Response preview: $(echo "$RESPONSE" | head -c 100)..."
else
    log_error "Unexpected server response"
    echo "$RESPONSE"
    EXIT_CODE=1
    exit 1
fi

#------------------------------------------------------------------------------
# Step 6: Run Jest tests
#------------------------------------------------------------------------------
log_step "Step 6: Running Jest test suite"

cd jest-tests

# Install dependencies if needed
if [ ! -d "node_modules" ]; then
    log_info "Installing npm dependencies..."
    npm install >> "$JEST_LOG" 2>&1
fi

log_info "Jest log: ${JEST_LOG}"
log_info "Running tests..."

# Run Jest with output to both console and log file in real-time
# Use stdbuf to disable buffering for immediate output
# Pass SERVER_URL to Jest tests
set +e
SERVER_URL="${SERVER_URL}" stdbuf -oL -eL npm test 2>&1 | tee "$JEST_LOG"
JEST_EXIT_CODE=${PIPESTATUS[0]}
set -e

if [ $JEST_EXIT_CODE -eq 0 ]; then
    log_ok "All Jest tests passed!"
else
    log_error "Some Jest tests failed (exit code: ${JEST_EXIT_CODE})"
    EXIT_CODE=1
fi

cd "$SCRIPT_DIR"

#------------------------------------------------------------------------------
# Summary
#------------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${CYAN}============================================================${NC}"
echo -e "${BOLD}                        SUMMARY${NC}"
echo -e "${BOLD}${CYAN}============================================================${NC}"
echo ""
echo -e "${BOLD}Log files:${NC}"
echo "  Build: ${BUILD_LOG}"
echo "  QEMU:  ${QEMU_LOG}"
echo "  Jest:  ${JEST_LOG}"
echo ""

if [ $EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}${BOLD}All tests passed!${NC}"
else
    echo -e "${RED}${BOLD}Some tests failed. Check logs above.${NC}"
    echo ""
    echo "To view QEMU output:"
    echo "  tail -100 ${QEMU_LOG}"
    echo ""
    echo "To view Jest output:"
    echo "  cat ${JEST_LOG}"
fi

exit $EXIT_CODE
