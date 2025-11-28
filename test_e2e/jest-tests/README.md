# E2E Test Suite for ESP32 HTTP Server

This directory contains comprehensive end-to-end tests for the ESP32 HTTP server running in QEMU.

## Setup

1. Install dependencies:
```bash
npm install
```

2. Start the QEMU server (in parent directory):
```bash
cd ..
./run_e2e_server.sh
```

3. Wait for server to start (you'll see "E2E Test Server is ready!" in the logs)

## Running Tests

Run all tests:
```bash
npm test
```

Run tests in watch mode:
```bash
npm run test:watch
```

Run tests with coverage:
```bash
npm run test:coverage
```

Run specific test file:
```bash
npm test http-routes
npm test websocket
npm test template
npm test static-files
npm test cors
npm test error-handling
```

## Test Structure

The test suite is organized by feature:

- **http-routes.test.ts**: Basic HTTP functionality (GET, POST, PUT, DELETE)
- **websocket.test.ts**: WebSocket connections, echo, and broadcast
- **template.test.ts**: Server-side template processing
- **static-files.test.ts**: Static file serving with correct MIME types
- **cors.test.ts**: Cross-Origin Resource Sharing headers and preflight
- **error-handling.test.ts**: 404s, malformed requests, security, and recovery

## Test Configuration

- Base URL: `http://localhost:8080` (configurable via `TEST_URL` env var)
- Timeout: 10 seconds per test
- Environment: Node.js with TypeScript

## QEMU Port Forwarding

The QEMU server runs with port forwarding:
- Host port 8080 → Guest port 80

This allows the Jest tests to connect to `localhost:8080` and reach the ESP32 server running inside QEMU.

## Troubleshooting

### Server not responding
- Check QEMU is running: `ps aux | grep qemu`
- Check port 8080 is listening: `lsof -i :8080`
- Restart QEMU: Kill existing process and run `./run_e2e_server.sh` again

### Tests failing
- Ensure server is fully started before running tests
- Check QEMU console for error messages
- Try running tests individually to isolate issues

### WebSocket tests failing
- WebSocket tests require the server to be running
- Some tests may be timing-sensitive, try increasing timeouts in jest.config

## Quick Test with Curl

Before running Jest tests, you can verify the server with curl:

```bash
cd ..
./test_curl.sh
```

This runs basic HTTP tests using curl commands.

## Coverage Report

After running tests with coverage, open the report:
```bash
open coverage/lcov-report/index.html
```

## Adding New Tests

1. Create a new file in `__tests__/` directory
2. Import axios and/or ws as needed
3. Follow the existing test patterns
4. Add descriptive test names and groups

Example:
```typescript
describe('New Feature', () => {
  it('should do something specific', async () => {
    const response = await axios.get('/new-endpoint');
    expect(response.status).toBe(200);
  });
});
```