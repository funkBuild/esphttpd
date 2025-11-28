# ESP32 HTTP Server E2E Test Environment

Complete end-to-end testing setup for the ESP32 HTTP server using QEMU emulation and Jest test framework.

## Overview

This directory contains a full E2E test server implementation and comprehensive test suite that:
- Runs an ESP32 HTTP server in QEMU with port forwarding
- Provides HTTP, WebSocket, template, and static file serving endpoints
- Includes Jest-based automated tests for all functionality
- Supports both manual (curl) and automated testing

## Quick Start

Run the complete E2E test suite with one command:
```bash
./run_full_e2e.sh
```

This will:
1. Build the ESP32 server
2. Start QEMU with port forwarding (host:8080 → guest:80)
3. Run curl tests
4. Install Jest dependencies (first run only)
5. Execute all Jest test suites
6. Clean up and report results

## Directory Structure

```
test_e2e/
├── main/
│   ├── main.c           # E2E test server implementation
│   └── CMakeLists.txt    # Component build configuration
├── jest-tests/           # Jest test suite
│   ├── __tests__/        # Test files
│   │   ├── http-routes.test.ts
│   │   ├── websocket.test.ts
│   │   ├── template.test.ts
│   │   ├── static-files.test.ts
│   │   ├── cors.test.ts
│   │   └── error-handling.test.ts
│   ├── package.json      # Jest dependencies
│   ├── tsconfig.json     # TypeScript configuration
│   ├── jest.setup.ts     # Test setup and helpers
│   └── README.md         # Jest test documentation
├── run_e2e_server.sh     # QEMU launcher with port forwarding
├── test_curl.sh          # Basic curl test script
├── run_full_e2e.sh       # Complete test runner
└── README.md             # This file
```

## Components

### 1. E2E Test Server (`main/main.c`)

Full-featured HTTP server with:

**HTTP Endpoints:**
- `GET /` - Home page
- `GET /api/status` - Server status (JSON)
- `GET /api/data/:id` - Get data by ID
- `POST /api/echo` - Echo POST body
- `PUT /api/update` - Update endpoint
- `DELETE /api/data/:id` - Delete by ID
- `GET /template` - Template engine demo
- `GET /headers` - Echo request headers
- `GET /cors` - CORS test endpoint
- `OPTIONS /cors` - CORS preflight
- `GET /static/*` - Static file serving

**WebSocket Endpoints:**
- `/ws/echo` - Echo WebSocket messages
- `/ws/broadcast` - Broadcast to all clients

### 2. QEMU Runner (`run_e2e_server.sh`)

- Builds the ESP32 application
- Creates merged flash image
- Launches QEMU with network port forwarding
- Enables serial console output
- Port forwarding: `localhost:8080` → ESP32:80

### 3. Curl Tests (`test_curl.sh`)

Quick validation script that tests:
- Server connectivity
- All HTTP methods (GET, POST, PUT, DELETE)
- API endpoints
- Template processing
- Header handling
- CORS support
- Static files
- 404 handling

### 4. Jest Test Suite (`jest-tests/`)

Comprehensive TypeScript test suite covering:
- **HTTP Routes**: All REST endpoints and methods
- **WebSocket**: Connection, echo, broadcast, binary data
- **Templates**: Variable substitution, dynamic content
- **Static Files**: MIME types, nested paths, security
- **CORS**: Headers, preflight, methods, origins
- **Error Handling**: 404s, malformed requests, recovery

## Usage Examples

### Run QEMU Server Only
```bash
cd test_e2e
./run_e2e_server.sh
```
Then access server at http://localhost:8080

### Run Curl Tests
With server running:
```bash
./test_curl.sh
```

### Run Jest Tests
```bash
cd jest-tests
npm install  # First time only
npm test     # Run all tests
npm test http-routes  # Run specific test
npm run test:watch    # Watch mode
npm run test:coverage # With coverage
```

### Run Everything
```bash
./run_full_e2e.sh           # Run all tests
./run_full_e2e.sh --coverage # Include coverage report
./run_full_e2e.sh --show-logs # Show QEMU logs
```

## Port Forwarding

QEMU networking is configured with user mode and port forwarding:
- Host port: 8080
- Guest port: 80
- Access URL: http://localhost:8080

This allows tests running on the host to connect to the ESP32 server running inside QEMU.

## Troubleshooting

### Port 8080 Already in Use
```bash
# Find process using port 8080
lsof -i :8080

# Kill it if it's an old QEMU instance
kill <PID>
```

### QEMU Won't Start
- Check ESP-IDF environment is sourced
- Verify QEMU ESP32 is installed
- Check build succeeded: `idf.py build`

### Tests Failing
- Ensure server is fully started (see "E2E Test Server is ready!" in logs)
- Check QEMU console for errors
- Try running tests individually
- Increase timeouts in jest.config if needed

### View QEMU Logs
```bash
# While QEMU is running
tail -f qemu.log

# After tests complete
cat qemu.log | grep ERROR
```

## CI/CD Integration

To run in CI/CD pipeline:
```bash
# Install dependencies
. $IDF_PATH/export.sh

# Run tests
cd test_e2e
./run_full_e2e.sh

# Check exit code
if [ $? -eq 0 ]; then
    echo "All tests passed"
else
    echo "Tests failed"
    exit 1
fi
```

## Development Tips

1. **Adding New Endpoints**: Edit `main/main.c`, add handler and route
2. **Adding New Tests**: Create new file in `jest-tests/__tests__/`
3. **Debugging**: Use `ESP_LOGI()` in server code, check qemu.log
4. **Performance**: QEMU is slower than hardware, adjust timeouts accordingly

## Requirements

- ESP-IDF v4.4 or later
- QEMU with ESP32-S3 support
- Node.js 16+ and npm
- curl (for basic tests)
- Standard build tools (gcc, make, etc.)

## License

Part of the esphttpd project.