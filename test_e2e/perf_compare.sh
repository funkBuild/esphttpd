#!/bin/bash
#
# Performance comparison: BSD sockets vs lwIP Raw TCP API
# Builds and benchmarks esphttpd in both transport modes under QEMU
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PORT=8088
SERVER_URL="http://127.0.0.1:${PORT}"
ENDPOINT="/perf"
DURATION=10
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="${SCRIPT_DIR}/logs"
RESULTS_FILE="${LOG_DIR}/perf_compare_${TIMESTAMP}.txt"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

QEMU_PID=""

log_info()  { echo -e "${CYAN}[INFO]${NC} $1"; }
log_ok()    { echo -e "${GREEN}[OK]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()  { echo -e "\n${BOLD}${CYAN}==> $1${NC}"; }

cleanup() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -9 "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    pkill -9 qemu-system-xtensa 2>/dev/null || true
}

trap cleanup EXIT

kill_qemu() {
    if pgrep -x qemu-system-xtensa > /dev/null 2>&1; then
        pkill -9 qemu-system-xtensa 2>/dev/null || true
        sleep 2
    fi
    if command -v lsof &>/dev/null && lsof -i :${PORT} -sTCP:LISTEN -t >/dev/null 2>&1; then
        kill -9 $(lsof -i :${PORT} -sTCP:LISTEN -t) 2>/dev/null || true
        sleep 2
    fi
}

wait_for_server() {
    local max_wait=60
    local waited=0
    while [ $waited -lt $max_wait ]; do
        if curl -s --fail --max-time 2 "${SERVER_URL}/" >/dev/null 2>&1; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

build_and_flash() {
    local mode=$1
    local build_log="${LOG_DIR}/build_${mode}_${TIMESTAMP}.log"

    log_step "Building in ${mode} mode"

    # Clean build directory for config switch
    rm -rf build/

    if [ "$mode" = "raw" ]; then
        # Create sdkconfig.defaults with raw API enabled
        cp sdkconfig.defaults sdkconfig.defaults.bak
        cat >> sdkconfig.defaults <<EOF

# Raw TCP API mode
CONFIG_HTTPD_USE_RAW_API=y
CONFIG_HTTPD_RAW_POLL_INTERVAL=4
CONFIG_LWIP_TCPIP_CORE_LOCKING=y
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=6144
EOF
    fi

    # Build
    if ! idf.py build > "$build_log" 2>&1; then
        log_error "Build failed in ${mode} mode! Check ${build_log}"
        if [ "$mode" = "raw" ]; then
            mv sdkconfig.defaults.bak sdkconfig.defaults
        fi
        tail -30 "$build_log"
        return 1
    fi

    # Restore original sdkconfig.defaults
    if [ "$mode" = "raw" ]; then
        mv sdkconfig.defaults.bak sdkconfig.defaults
    fi

    # Create flash image
    esptool.py --chip esp32s3 merge_bin \
        -o build/merged.bin \
        --flash_mode dio \
        --flash_size 4MB \
        0x0 build/bootloader/bootloader.bin \
        0x8000 build/partition_table/partition-table.bin \
        0x10000 build/esphttpd_e2e_test.bin \
        >> "$build_log" 2>&1

    dd if=/dev/zero of=build/flash.bin bs=1M count=4 2>/dev/null
    dd if=build/merged.bin of=build/flash.bin conv=notrunc 2>/dev/null

    log_ok "Build successful (${mode} mode)"
    return 0
}

start_qemu() {
    local mode=$1
    local qemu_log="${LOG_DIR}/qemu_${mode}_${TIMESTAMP}.log"

    log_step "Starting QEMU (${mode} mode)"
    kill_qemu

    qemu-system-xtensa \
        -nographic \
        -machine esp32s3 \
        -drive file=build/flash.bin,if=mtd,format=raw \
        -nic user,model=open_eth,hostfwd=tcp:127.0.0.1:${PORT}-:80 \
        -serial mon:stdio \
        -no-reboot \
        > "$qemu_log" 2>&1 &

    QEMU_PID=$!

    if ! wait_for_server; then
        log_error "Server failed to start in ${mode} mode"
        tail -30 "$qemu_log"
        return 1
    fi

    log_ok "Server ready (${mode} mode)"
    return 0
}

run_benchmark() {
    local mode=$1
    local label=$2

    log_step "Benchmarking ${label}"

    # Warmup
    for i in $(seq 1 20); do
        curl -s "${SERVER_URL}${ENDPOINT}" > /dev/null 2>&1
    done

    echo "" >> "$RESULTS_FILE"
    echo "=== ${label} ===" >> "$RESULTS_FILE"
    echo "" >> "$RESULTS_FILE"

    for concurrency in 1 4 8; do
        echo "--- Concurrency: ${concurrency} ---" | tee -a "$RESULTS_FILE"

        if command -v ab &> /dev/null; then
            # Use Apache Bench
            ab -t $DURATION -c $concurrency -q "${SERVER_URL}${ENDPOINT}" 2>/dev/null | \
                grep -E "(Requests per second|Time per request|Complete requests|Failed requests)" | \
                tee -a "$RESULTS_FILE"
        else
            # Fallback: Python benchmark
            HOST="${SERVER_URL}" python3 -c "
import urllib.request, time, threading, statistics
from concurrent.futures import ThreadPoolExecutor

url = '${SERVER_URL}${ENDPOINT}'
duration = ${DURATION}
concurrency = ${concurrency}
success = 0
errors = 0
latencies = []
lock = threading.Lock()

def make_request():
    global success, errors
    try:
        start = time.perf_counter()
        req = urllib.request.urlopen(url, timeout=5)
        _ = req.read()
        lat = (time.perf_counter() - start) * 1000
        with lock:
            success += 1
            latencies.append(lat)
    except:
        with lock:
            errors += 1

start_time = time.time()
end_time = start_time + duration

with ThreadPoolExecutor(max_workers=concurrency) as executor:
    futures = []
    while time.time() < end_time:
        active = len([f for f in futures if not f.done()])
        for _ in range(concurrency - active):
            if time.time() >= end_time:
                break
            futures.append(executor.submit(make_request))
        time.sleep(0.001)
    for f in futures:
        f.result()

elapsed = time.time() - start_time
rps = success / elapsed
sorted_lat = sorted(latencies)
p50 = sorted_lat[len(sorted_lat)//2] if sorted_lat else 0
p95 = sorted_lat[int(len(sorted_lat)*0.95)] if sorted_lat else 0
p99 = sorted_lat[int(len(sorted_lat)*0.99)] if sorted_lat else 0
avg = statistics.mean(latencies) if latencies else 0

print(f'Complete requests:    {success}')
print(f'Failed requests:      {errors}')
print(f'Requests per second:  {rps:.2f} [#/sec]')
print(f'Time per request:     {avg:.3f} [ms] (mean)')
print(f'P50 latency:          {p50:.3f} [ms]')
print(f'P95 latency:          {p95:.3f} [ms]')
print(f'P99 latency:          {p99:.3f} [ms]')
" 2>&1 | tee -a "$RESULTS_FILE"
        fi
        echo "" | tee -a "$RESULTS_FILE"
    done
}

stop_qemu() {
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -9 "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    QEMU_PID=""
    sleep 2
}

#==============================================================================
# Main
#==============================================================================

echo -e "${BOLD}${CYAN}"
echo "============================================================"
echo "   esphttpd Performance: Sockets vs Raw TCP API"
echo "============================================================"
echo -e "${NC}"

mkdir -p "$LOG_DIR"

echo "Performance Comparison: Sockets vs Raw TCP API" > "$RESULTS_FILE"
echo "Date: $(date)" >> "$RESULTS_FILE"
echo "Duration per test: ${DURATION}s" >> "$RESULTS_FILE"
echo "Endpoint: ${ENDPOINT}" >> "$RESULTS_FILE"

# ---- Socket Mode ----
if build_and_flash "socket"; then
    if start_qemu "socket"; then
        run_benchmark "socket" "BSD Sockets + select()"
        stop_qemu
    fi
fi

# ---- Raw TCP API Mode ----
if build_and_flash "raw"; then
    if start_qemu "raw"; then
        run_benchmark "raw" "lwIP Raw TCP API"
        stop_qemu
    fi
fi

# ---- Summary ----
echo ""
echo -e "${BOLD}${CYAN}============================================================${NC}"
echo -e "${BOLD}                    RESULTS${NC}"
echo -e "${BOLD}${CYAN}============================================================${NC}"
echo ""
cat "$RESULTS_FILE"
echo ""
echo -e "Full results: ${RESULTS_FILE}"
