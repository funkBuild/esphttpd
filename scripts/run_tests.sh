#!/bin/bash
#
# esphttpd Test Runner
# Runs unit tests and e2e tests under QEMU and reports unified results
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
UNIT_TEST_DIR="$PROJECT_DIR/test_app"
E2E_TEST_DIR="$PROJECT_DIR/test_e2e"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
UNIT_TESTS_PASSED=0
UNIT_TESTS_FAILED=0
E2E_TESTS_PASSED=0
E2E_TESTS_FAILED=0

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    pkill -9 qemu-system-xtensa 2>/dev/null || true
}
trap cleanup EXIT

# Print a section header
print_header() {
    echo ""
    echo "============================================================"
    echo "$1"
    echo "============================================================"
}

# Print Unity-style test result
print_unity_result() {
    local test_name="$1"
    local status="$2"
    local message="${3:-}"

    if [ "$status" = "PASS" ]; then
        echo -e "${GREEN}TEST($test_name):PASS${NC}"
    else
        echo -e "${RED}TEST($test_name):FAIL${NC}"
        if [ -n "$message" ]; then
            echo "  $message"
        fi
    fi
}

# Build a test project
build_test_project() {
    local dir="$1"
    local name="$2"

    print_header "Building $name"
    cd "$dir"

    if ! idf.py build 2>&1; then
        echo -e "${RED}Build failed for $name${NC}"
        return 1
    fi

    echo -e "${GREEN}Build successful for $name${NC}"
    return 0
}

# Create QEMU flash image
create_qemu_image() {
    local build_dir="$1"
    local bin_name="$2"

    cd "$build_dir"

    esptool.py --chip=esp32s3 merge_bin --output=qemu_flash.bin \
        --fill-flash-size=2MB --flash_mode dio --flash_freq 80m --flash_size 2MB \
        0x0 bootloader/bootloader.bin \
        0x10000 "$bin_name" \
        0x8000 partition_table/partition-table.bin

    return $?
}

# Run unit tests under QEMU
run_unit_tests() {
    print_header "Running Unit Tests"

    local output_file="/tmp/esphttpd_unit_tests.txt"

    cd "$UNIT_TEST_DIR/build"

    # Kill any existing QEMU
    pkill -9 qemu-system-xtensa 2>/dev/null || true
    sleep 1

    # Create efuse file if it doesn't exist
    if [ ! -f qemu_efuse.bin ]; then
        dd if=/dev/zero of=qemu_efuse.bin bs=1K count=4 2>/dev/null
    fi

    # Verify flash image exists
    if [ ! -f qemu_flash.bin ]; then
        echo -e "${RED}ERROR: qemu_flash.bin not found${NC}"
        return 1
    fi

    echo "Starting QEMU for unit tests..."
    timeout 180 qemu-system-xtensa -M esp32s3 \
        -drive file=qemu_flash.bin,if=mtd,format=raw \
        -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
        -global driver=nvram.esp32c3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -nic user,model=open_eth \
        -nographic 2>&1 | tee "$output_file" || true

    # Parse Unity output
    echo ""
    echo "--- Unit Test Results ---"

    # Extract test results - sanitize to ensure clean integers
    local passed_raw=$(grep -c ":PASS" "$output_file" 2>/dev/null) || passed_raw="0"
    local failed_raw=$(grep -c ":FAIL" "$output_file" 2>/dev/null) || failed_raw="0"
    local ignored_raw=$(grep -c ":IGNORE" "$output_file" 2>/dev/null) || ignored_raw="0"

    # Sanitize to clean integers
    local passed=$(echo "$passed_raw" | tr -cd '0-9')
    local failed=$(echo "$failed_raw" | tr -cd '0-9')
    local ignored=$(echo "$ignored_raw" | tr -cd '0-9')

    # Default to 0 if empty
    passed=${passed:-0}
    failed=${failed:-0}
    ignored=${ignored:-0}

    # Check for overall pass
    if grep -q "QEMU_TEST_COMPLETE: PASS" "$output_file"; then
        echo -e "${GREEN}Unit Tests: PASSED${NC}"
        UNIT_TESTS_PASSED=$passed
        UNIT_TESTS_FAILED=$failed
        return 0
    else
        echo -e "${RED}Unit Tests: FAILED${NC}"
        UNIT_TESTS_PASSED=$passed
        UNIT_TESTS_FAILED=$((failed + 1))  # At least one failure
        return 1
    fi
}

# Run e2e tests
run_e2e_tests() {
    print_header "Running E2E Tests"

    local qemu_log="/tmp/esphttpd_e2e_qemu.txt"
    local jest_output="/tmp/esphttpd_e2e_jest.txt"

    cd "$E2E_TEST_DIR/build"

    # Kill any existing QEMU
    pkill -9 qemu-system-xtensa 2>/dev/null || true
    sleep 1

    # Create efuse file if it doesn't exist
    if [ ! -f qemu_efuse.bin ]; then
        dd if=/dev/zero of=qemu_efuse.bin bs=1K count=4 2>/dev/null
    fi

    # Verify flash image exists
    if [ ! -f qemu_flash.bin ]; then
        echo -e "${RED}ERROR: qemu_flash.bin not found${NC}"
        return 1
    fi

    echo "Starting QEMU server for e2e tests..."
    qemu-system-xtensa -M esp32s3 \
        -drive file=qemu_flash.bin,if=mtd,format=raw \
        -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
        -global driver=nvram.esp32c3.efuse,property=drive,value=efuse \
        -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
        -nic user,model=open_eth,hostfwd=tcp::10080-:80 \
        -nographic > "$qemu_log" 2>&1 &

    local qemu_pid=$!

    echo "Waiting for server to start..."
    sleep 15

    # Check if server is ready
    local retries=5
    while [ $retries -gt 0 ]; do
        if curl -s --max-time 5 http://127.0.0.1:10080/hello > /dev/null 2>&1; then
            echo "Server is ready!"
            break
        fi
        echo "Waiting for server... ($retries retries left)"
        sleep 5
        retries=$((retries - 1))
    done

    if [ $retries -eq 0 ]; then
        echo -e "${RED}Server failed to start${NC}"
        kill $qemu_pid 2>/dev/null || true
        E2E_TESTS_FAILED=1
        return 1
    fi

    # Run Jest tests
    echo "Running Jest e2e tests..."
    cd "$E2E_TEST_DIR/jest-tests"

    # Run tests and capture output
    SERVER_URL=http://127.0.0.1:10080 npm test -- --json --outputFile="$jest_output.json" 2>&1 | tee "$jest_output" || true

    # Kill QEMU
    pkill -9 qemu-system-xtensa 2>/dev/null || true

    # Parse Jest output and convert to Unity format
    echo ""
    echo "--- E2E Test Results ---"

    # Parse Jest JSON output if available
    if [ -f "$jest_output.json" ]; then
        local num_passed=$(jq -r '.numPassedTests // 0' "$jest_output.json" 2>/dev/null | tr -d '\n' || echo "0")
        local num_failed=$(jq -r '.numFailedTests // 0' "$jest_output.json" 2>/dev/null | tr -d '\n' || echo "0")
        local success=$(jq -r '.success // false' "$jest_output.json" 2>/dev/null | tr -d '\n' || echo "false")

        E2E_TESTS_PASSED=${num_passed:-0}
        E2E_TESTS_FAILED=${num_failed:-0}

        # Print individual test results in Unity format
        jq -r '.testResults[]?.assertionResults[]? | "TEST(\(.ancestorTitles | join(" > ")) > \(.title)):\(if .status == "passed" then "PASS" else "FAIL" end)"' "$jest_output.json" 2>/dev/null || true

        if [ "$success" = "true" ]; then
            echo -e "${GREEN}E2E Tests: PASSED${NC}"
            return 0
        else
            echo -e "${RED}E2E Tests: FAILED${NC}"
            return 1
        fi
    else
        # Fallback: parse text output
        local passed=$(grep -c "✓" "$jest_output" 2>/dev/null || echo "0")
        local failed=$(grep -c "✕" "$jest_output" 2>/dev/null || echo "0")
        E2E_TESTS_PASSED=$passed
        E2E_TESTS_FAILED=$failed

        if [ "$failed" -eq 0 ] && [ "$passed" -gt 0 ]; then
            echo -e "${GREEN}E2E Tests: PASSED${NC}"
            return 0
        else
            echo -e "${RED}E2E Tests: FAILED${NC}"
            return 1
        fi
    fi
}

# Print final summary
print_summary() {
    # Sanitize all values to ensure they're clean integers
    local unit_passed=$(echo "$UNIT_TESTS_PASSED" | tr -cd '0-9')
    local unit_failed=$(echo "$UNIT_TESTS_FAILED" | tr -cd '0-9')
    local e2e_passed=$(echo "$E2E_TESTS_PASSED" | tr -cd '0-9')
    local e2e_failed=$(echo "$E2E_TESTS_FAILED" | tr -cd '0-9')

    # Default to 0 if empty
    unit_passed=${unit_passed:-0}
    unit_failed=${unit_failed:-0}
    e2e_passed=${e2e_passed:-0}
    e2e_failed=${e2e_failed:-0}

    local total_passed=$((unit_passed + e2e_passed))
    local total_failed=$((unit_failed + e2e_failed))
    local total=$((total_passed + total_failed))

    print_header "Test Summary"

    echo ""
    echo "Unit Tests:"
    echo "  Passed: $unit_passed"
    echo "  Failed: $unit_failed"
    echo ""
    echo "E2E Tests:"
    echo "  Passed: $e2e_passed"
    echo "  Failed: $e2e_failed"
    echo ""
    echo "============================================================"
    echo "TOTAL: $total_passed passed, $total_failed failed ($total tests)"
    echo "============================================================"

    if [ $total_failed -eq 0 ]; then
        echo -e "${GREEN}ALL TESTS PASSED${NC}"
        return 0
    else
        echo -e "${RED}SOME TESTS FAILED${NC}"
        return 1
    fi
}

# Main execution
main() {
    local unit_result=0
    local e2e_result=0

    print_header "esphttpd Test Suite"
    echo "Project: $PROJECT_DIR"
    echo "Date: $(date)"

    # Build unit tests
    if ! build_test_project "$UNIT_TEST_DIR" "Unit Tests"; then
        echo -e "${RED}Failed to build unit tests${NC}"
        exit 1
    fi

    # Create unit test QEMU image
    if ! create_qemu_image "$UNIT_TEST_DIR/build" "esphttpd_test_app.bin"; then
        echo -e "${RED}Failed to create unit test QEMU image${NC}"
        exit 1
    fi

    # Build e2e tests
    if ! build_test_project "$E2E_TEST_DIR" "E2E Tests"; then
        echo -e "${RED}Failed to build e2e tests${NC}"
        exit 1
    fi

    # Create e2e test QEMU image
    if ! create_qemu_image "$E2E_TEST_DIR/build" "esphttpd_e2e_test.bin"; then
        echo -e "${RED}Failed to create e2e test QEMU image${NC}"
        exit 1
    fi

    # Run unit tests
    run_unit_tests || unit_result=1

    # Run e2e tests
    run_e2e_tests || e2e_result=1

    # Print summary
    print_summary

    # Return appropriate exit code
    if [ $unit_result -ne 0 ] || [ $e2e_result -ne 0 ]; then
        exit 1
    fi

    exit 0
}

# Run main
main "$@"
