#!/bin/bash

# Performance test for esphttpd E2E server
# Measures requests per second over a 5 second period

HOST="${HOST:-http://localhost:8080}"
DURATION=5
ENDPOINT="/perf"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${CYAN}${BOLD}==================== HTTP Performance Test ====================${NC}"
echo -e "Host: ${HOST}"
echo -e "Endpoint: ${ENDPOINT}"
echo -e "Duration: ${DURATION} seconds"
echo ""

# Check if server is running
if ! curl -s --fail --connect-timeout 2 "$HOST/perf" > /dev/null 2>&1; then
    echo -e "${RED}Server not responding at $HOST${NC}"
    echo "Make sure the E2E server is running with: ./run_e2e_server.sh"
    exit 1
fi

# Check for Apache Bench
if command -v ab &> /dev/null; then
    echo -e "${GREEN}Using Apache Bench (ab) for testing...${NC}"
    echo ""

    # Run ab with concurrency 1 for sequential baseline
    echo -e "${YELLOW}Test 1: Sequential requests (concurrency=1)${NC}"
    ab -t $DURATION -c 1 -q "$HOST$ENDPOINT" 2>/dev/null | grep -E "(Requests per second|Time per request|Transfer rate|Complete requests|Failed requests)"
    echo ""

    # Run ab with concurrency 4
    echo -e "${YELLOW}Test 2: Concurrent requests (concurrency=4)${NC}"
    ab -t $DURATION -c 4 -q "$HOST$ENDPOINT" 2>/dev/null | grep -E "(Requests per second|Time per request|Transfer rate|Complete requests|Failed requests)"
    echo ""

    # Run ab with concurrency 8
    echo -e "${YELLOW}Test 3: High concurrency (concurrency=8)${NC}"
    ab -t $DURATION -c 8 -q "$HOST$ENDPOINT" 2>/dev/null | grep -E "(Requests per second|Time per request|Transfer rate|Complete requests|Failed requests)"

else
    # Fallback to curl-based benchmark
    echo -e "${YELLOW}Apache Bench not found, using curl-based benchmark...${NC}"
    echo ""

    # Count requests in a loop
    count=0
    errors=0
    start_time=$(date +%s.%N)
    end_time=$(echo "$start_time + $DURATION" | bc)

    while (( $(echo "$(date +%s.%N) < $end_time" | bc -l) )); do
        response=$(curl -s -w "%{http_code}" -o /dev/null "$HOST$ENDPOINT" 2>/dev/null)
        if [ "$response" = "200" ]; then
            ((count++))
        else
            ((errors++))
        fi
    done

    actual_duration=$(echo "$(date +%s.%N) - $start_time" | bc)
    rps=$(echo "scale=2; $count / $actual_duration" | bc)

    echo -e "${CYAN}Results:${NC}"
    echo "  Complete requests: $count"
    echo "  Failed requests:   $errors"
    echo "  Duration:          ${actual_duration}s"
    echo -e "  ${BOLD}Requests/second:   ${rps}${NC}"
fi

echo ""
echo -e "${CYAN}${BOLD}==================== Test Complete ====================${NC}"
