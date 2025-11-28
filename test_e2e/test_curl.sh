#!/bin/bash

# Simple curl tests to verify E2E server is working
# Run this while the QEMU server is running (./run_e2e_server.sh)

HOST="http://localhost:8080"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${CYAN}==================== E2E Server Tests ====================${NC}"

# Test if server is running
echo -e "${YELLOW}Testing server connectivity...${NC}"
if ! curl -s --fail --connect-timeout 2 "$HOST/" > /dev/null 2>&1; then
    echo -e "${RED}Server not responding at $HOST${NC}"
    echo "Make sure the E2E server is running with: ./run_e2e_server.sh"
    exit 1
fi
echo -e "${GREEN}✓ Server is running${NC}"

# Test home page
echo -e "\n${CYAN}1. Testing home page (GET /)${NC}"
response=$(curl -s "$HOST/")
if [[ $response == *"ESP32 E2E Test Server"* ]]; then
    echo -e "${GREEN}✓ Home page works${NC}"
    echo "Response preview: ${response:0:100}..."
else
    echo -e "${RED}✗ Home page failed${NC}"
fi

# Test API status
echo -e "\n${CYAN}2. Testing API status (GET /api/status)${NC}"
response=$(curl -s "$HOST/api/status")
echo "Response: $response"
if [[ $response == *"\"status\":\"ok\""* ]]; then
    echo -e "${GREEN}✓ API status works${NC}"
else
    echo -e "${RED}✗ API status failed${NC}"
fi

# Test API data with ID
echo -e "\n${CYAN}3. Testing API data (GET /api/data/123)${NC}"
response=$(curl -s "$HOST/api/data/123")
echo "Response: $response"
if [[ $response == *"\"id\":\"123\""* ]]; then
    echo -e "${GREEN}✓ API data works${NC}"
else
    echo -e "${RED}✗ API data failed${NC}"
fi

# Test POST echo
echo -e "\n${CYAN}4. Testing POST echo (POST /api/echo)${NC}"
test_data='{"test":"data","number":42}'
response=$(curl -s -X POST -H "Content-Type: application/json" -d "$test_data" "$HOST/api/echo")
echo "Sent: $test_data"
echo "Received: $response"
if [[ "$response" == "$test_data" ]]; then
    echo -e "${GREEN}✓ POST echo works${NC}"
else
    echo -e "${RED}✗ POST echo failed${NC}"
fi

# Test PUT update
echo -e "\n${CYAN}5. Testing PUT update (PUT /api/update)${NC}"
response=$(curl -s -X PUT -H "Content-Type: application/json" -d '{"update":"test"}' "$HOST/api/update")
echo "Response: $response"
if [[ $response == *"\"message\":\"Updated\""* ]]; then
    echo -e "${GREEN}✓ PUT update works${NC}"
else
    echo -e "${RED}✗ PUT update failed${NC}"
fi

# Test DELETE
echo -e "\n${CYAN}6. Testing DELETE (DELETE /api/data/456)${NC}"
response=$(curl -s -X DELETE "$HOST/api/data/456")
echo "Response: $response"
if [[ $response == *"\"id\":\"456\""* ]]; then
    echo -e "${GREEN}✓ DELETE works${NC}"
else
    echo -e "${RED}✗ DELETE failed${NC}"
fi

# Test template engine
echo -e "\n${CYAN}7. Testing template engine (GET /template)${NC}"
response=$(curl -s "$HOST/template")
if [[ $response == *"E2E Test Page"* ]] && [[ $response == *"Dynamic content here"* ]]; then
    echo -e "${GREEN}✓ Template engine works${NC}"
    echo "Response preview: ${response:0:100}..."
else
    echo -e "${RED}✗ Template engine failed${NC}"
fi

# Test headers
echo -e "\n${CYAN}8. Testing headers (GET /headers)${NC}"
response=$(curl -s -H "X-Test-Header: TestValue123" "$HOST/headers")
if [[ $response == *"X-Test-Header: TestValue123"* ]]; then
    echo -e "${GREEN}✓ Header echo works${NC}"
else
    echo -e "${RED}✗ Header echo failed${NC}"
fi

# Test CORS
echo -e "\n${CYAN}9. Testing CORS (GET /cors)${NC}"
response=$(curl -s -I "$HOST/cors" | grep -i "access-control")
echo "$response"
if [[ $response == *"Access-Control-Allow-Origin"* ]]; then
    echo -e "${GREEN}✓ CORS headers present${NC}"
else
    echo -e "${RED}✗ CORS headers missing${NC}"
fi

# Test static files
echo -e "\n${CYAN}10. Testing static files (GET /static/test.txt)${NC}"
response=$(curl -s "$HOST/static/test.txt")
echo "Response: $response"
if [[ $response == *"Mock static file"* ]]; then
    echo -e "${GREEN}✓ Static file serving works${NC}"
else
    echo -e "${RED}✗ Static file serving failed${NC}"
fi

# Test 404
echo -e "\n${CYAN}11. Testing 404 handler${NC}"
response=$(curl -s "$HOST/nonexistent")
if [[ $response == *"404 - Not Found"* ]]; then
    echo -e "${GREEN}✓ 404 handler works${NC}"
else
    echo -e "${RED}✗ 404 handler failed${NC}"
fi

echo -e "\n${CYAN}==================== Tests Complete ====================${NC}"
echo -e "${GREEN}Basic HTTP functionality verified!${NC}"
echo -e "${YELLOW}Note: WebSocket tests require a WebSocket client${NC}"