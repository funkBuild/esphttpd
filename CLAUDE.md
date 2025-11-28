# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

esphttpd is a high-performance HTTP web server for ESP32 microcontrollers, built on top of ESP-IDF (Espressif IoT Development Framework). It provides HTTP routing, WebSocket support, and filesystem serving capabilities using lwIP networking and FreeRTOS.

This is an ESP-IDF component intended to be used as a git submodule in ESP32 projects.

## Build System

This project uses ESP-IDF's component build system (CMake-based).

**Building**: This component is meant to be included in an ESP-IDF project as a submodule. It cannot be built standalone. To test changes:
1. Add as submodule to an ESP-IDF project: `git submodule add <url> components/esphttpd`
2. Build the parent project: `idf.py build`
3. Flash to device: `idf.py flash`
4. Monitor output: `idf.py monitor`

**Dependencies**: Requires ESP-IDF v3.1.2 or later with:
- mbedtls (for base64 encoding and SHA1)
- esp_ringbuf (for WebSocket message queuing)
- esp_timer (for timing operations)
- lwIP (for networking)
- FreeRTOS (for task management)

## Code Architecture

### Threading Model

The server uses an event-driven architecture:
- **Server task** (`server_task`): FreeRTOS task that runs the event loop
- **Event loop**: Uses `select()` for multiplexed I/O across all connections
- **Connection pool**: Pre-allocated pool of connection contexts for zero-allocation request handling
- **HTTP requests**: Processed synchronously within the event loop
- **WebSocket connections**: Managed as long-lived connections within the same event loop

### Key Components

**Request Handling Flow**:
1. Event loop accepts connection via `accept()` on listening socket
2. Connection added to pool, state set to `CONN_STATE_HTTP_HEADERS`
3. `on_http_request` callback parses headers using streaming parser
4. Route matched via radix tree lookup
5. Handler invoked with `httpd_req_t*` context
6. Response sent via `httpd_resp_*` functions
7. Connection recycled or kept alive based on headers

**Route Matching**:
- HTTP routes: Radix tree for O(log n) lookup with pattern support
- WebSocket routes: Array with pattern matching
- Pattern support: `*` matches any sequence, `:param` extracts URL parameters

**WebSocket Architecture**:
- Frame parsing via state machine in `ws_frame_context_t`
- Pub/sub channels with bitmask-based subscription tracking
- Channel hash table for O(1) channel lookup
- Connection count tracking via `httpd_ws_get_connection_count()`

**Memory Management**:
- Connection pool pre-allocates all connection contexts
- Per-request header buffer (2KB) for header storage
- Body data streamed via `httpd_req_recv()`
- File uploads via `httpd_req_pipe_to_file()` for streaming writes

### Important Data Structures

**httpd_req_t** (esphttpd.h): Request context with:
- `fd`: Socket file descriptor
- `method`: HTTP method enum
- `uri`, `path`, `query`: URL components
- `params[]`: Extracted route parameters from `:param` patterns
- Use `httpd_req_get_*` accessors for safe access

**httpd_ws_t** (esphttpd.h): WebSocket connection context with:
- `fd`: Socket file descriptor
- `user_data`: User-defined context pointer
- `connected`: Connection state flag

**httpd_router_t** (esphttpd.h): Router for Express-like nested routing with:
- Route registration via `httpd_router_get()`, `httpd_router_post()`, etc.
- Middleware support via `httpd_router_use()`
- Mount at prefix via `httpd_mount()`

## Common Patterns

**Starting the Server**:
```c
httpd_handle_t server = NULL;
httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.port = 80;
httpd_start(&server, &config);
```

**Adding HTTP Routes**:
```c
httpd_register_route(server, &(httpd_route_t){
    .method = HTTP_GET,
    .pattern = "/api/status",
    .handler = handle_status
});
```

**HTTP Handler Pattern**:
```c
static httpd_err_t handle_status(httpd_req_t* req) {
    const char* json = "{\"status\":\"ok\"}";
    httpd_resp_set_status(req, 200);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return HTTPD_OK;
}
```

**Reading Request Body**:
```c
char buf[1024];
int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
if (received > 0) {
    buf[received] = '\0';
    // Process body...
}
```

**Adding WebSocket Routes**:
```c
httpd_register_ws_route(server, &(httpd_ws_route_t){
    .pattern = "/ws/echo",
    .handler = ws_echo_handler
});
```

**WebSocket Handler Pattern**:
```c
static httpd_err_t ws_echo_handler(httpd_ws_t* ws, httpd_ws_event_t* event) {
    switch (event->type) {
        case WS_EVENT_CONNECT:
            // Connection accepted by returning HTTPD_OK
            httpd_ws_send_text(ws, "Welcome!");
            break;
        case WS_EVENT_MESSAGE:
            // Echo back with same frame type
            httpd_ws_send(ws, event->data, event->len, event->frame_type);
            break;
        case WS_EVENT_DISCONNECT:
            // Cleanup if needed
            break;
        case WS_EVENT_ERROR:
            break;
    }
    return HTTPD_OK;
}
```

**WebSocket Channel (Pub/Sub)**:
```c
// Subscribe to a channel
httpd_ws_join(ws, "chat-room");

// Publish to channel subscribers
httpd_ws_publish(server, "chat-room", msg, len, WS_TYPE_TEXT);

// Unsubscribe
httpd_ws_leave(ws, "chat-room");
```

**Sending WebSocket Messages**:
- `httpd_ws_send(ws, data, len, WS_TYPE_TEXT)`: Send to single connection
- `httpd_ws_send_text(ws, text)`: Send text message
- `httpd_ws_broadcast(server, pattern, data, len, WS_TYPE_TEXT)`: Broadcast to route
- `httpd_ws_publish(server, channel, data, len, WS_TYPE_TEXT)`: Publish to channel

**Using Routers (Express-style)**:
```c
// Create router
httpd_router_t api = httpd_router_create();

// Add routes to router
httpd_router_get(api, "/status", handle_status);
httpd_router_post(api, "/data", handle_data);

// Add middleware
httpd_router_use(api, auth_middleware);

// Mount at prefix
httpd_mount(server, "/api/v1", api);
```

## File Structure

```
esphttpd/
├── include/
│   └── esphttpd.h          # Public API (only header users should include)
├── src/
│   ├── private/            # Internal headers (do not use in application code)
│   │   ├── connection.h    # Connection pool management
│   │   ├── event_loop.h    # Event loop and handlers
│   │   ├── http_parser.h   # HTTP request parsing
│   │   ├── websocket.h     # WebSocket frame handling
│   │   ├── radix_tree.h    # Radix tree router
│   │   ├── router.h        # Router types
│   │   ├── template.h      # Template processing
│   │   ├── filesystem.h    # LittleFS file serving
│   │   └── test_exports.h  # Internal test exports
│   ├── esphttpd.c          # Main API implementation
│   ├── connection.c        # Connection pool
│   ├── event_loop.c        # Event loop (select-based)
│   ├── http_parser.c       # Streaming HTTP parser
│   ├── websocket.c         # WebSocket protocol
│   ├── radix_tree.c        # Radix tree for routing
│   ├── router.c            # URL routing
│   ├── template.c          # Template variable substitution
│   └── filesystem.c        # LittleFS integration
├── examples/
│   └── example_app.c       # Example application
├── test_app/               # Unit tests (ESP-IDF project)
├── test_e2e/               # End-to-end tests (ESP-IDF project)
├── CMakeLists.txt          # ESP-IDF component configuration
└── component.mk            # Legacy build system support
```

## Testing Considerations

When making changes:
1. Test with multiple concurrent connections
2. Verify proper cleanup on connection close
3. Test route matching with wildcards and parameters
4. Ensure WebSocket pub/sub channels work correctly
5. Test large file uploads via `httpd_req_pipe_to_file()`
6. Verify CORS headers work correctly

## Running Tests

### Unit Tests (QEMU)

The test suite runs under QEMU emulation. From the `test_app` directory:

```bash
# Kill any existing QEMU processes first
pkill -9 qemu-system-xtensa

# Build and run tests
cd test_app
idf.py build

# Create QEMU flash image
cd build
esptool.py --chip=esp32s3 merge_bin --output=qemu_flash.bin \
    --fill-flash-size=2MB --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x0 bootloader/bootloader.bin \
    0x10000 esphttpd_test_app.bin \
    0x8000 partition_table/partition-table.bin

# Run QEMU (timeout after 3 minutes)
timeout 180 qemu-system-xtensa -M esp32s3 \
    -drive file=qemu_flash.bin,if=mtd,format=raw \
    -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32c3.efuse,property=drive,value=efuse \
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
    -nic user,model=open_eth \
    -nographic 2>&1 | tee /tmp/qemu_out.txt
```

Look for `QEMU_TEST_COMPLETE: PASS` in output.

### Performance Benchmarks

The test suite includes performance benchmarks that measure critical operations:

```bash
# After running tests, extract performance results:
strings /tmp/qemu_out.txt | grep "^PERF:"
```

**Benchmark categories:**
- HTTP Parser: method parsing, header identification, request parsing
- Radix Router: static/param/deep lookups, route insertion
- WebSocket: frame masking, header building, frame processing
- Template: plain text and variable substitution
- Connection Pool: find, index lookup, active count
- Utilities: MIME type lookup, status text

**Baseline results** are stored in `test_app/PERF_BASELINE.md` for comparison.

## Key Constants

- `MAX_CONNECTIONS`: 16 (connection.h)
- `MAX_HTTP_ROUTES`: 64 (esphttpd.c)
- `MAX_WS_ROUTES`: 16 (esphttpd.c)
- `HTTPD_WS_MAX_CHANNELS`: 32 (esphttpd.h)
- `REQ_HEADER_BUF_SIZE`: 2048 bytes (esphttpd.c)

## API Quick Reference

### Server Lifecycle
- `httpd_start(handle, config)` - Start server
- `httpd_stop(handle)` - Stop server
- `httpd_is_running(handle)` - Check if running

### Routes
- `httpd_register_route(handle, route)` - Register HTTP route
- `httpd_register_ws_route(handle, route)` - Register WebSocket route
- `httpd_unregister_route(handle, method, pattern)` - Remove route

### Request (httpd_req_t*)
- `httpd_req_get_method(req)` - Get HTTP method
- `httpd_req_get_uri(req)` - Get full URI
- `httpd_req_get_path(req)` - Get path without query
- `httpd_req_get_header(req, key)` - Get header value
- `httpd_req_get_param(req, key)` - Get route parameter
- `httpd_req_get_query(req, key, buf, size)` - Get query parameter
- `httpd_req_recv(req, buf, len)` - Receive body data
- `httpd_req_pipe_to_file(req, path)` - Stream body to file

### Response
- `httpd_resp_set_status(req, code)` - Set status code
- `httpd_resp_set_header(req, key, value)` - Set header
- `httpd_resp_set_type(req, type)` - Set Content-Type
- `httpd_resp_send(req, body, len)` - Send response
- `httpd_resp_send_chunk(req, chunk, len)` - Send chunked
- `httpd_resp_send_error(req, status, msg)` - Send error
- `httpd_resp_send_json(req, json)` - Send JSON

### WebSocket
- `httpd_ws_send(ws, data, len, type)` - Send message
- `httpd_ws_send_text(ws, text)` - Send text
- `httpd_ws_broadcast(handle, pattern, data, len, type)` - Broadcast
- `httpd_ws_close(ws, code, reason)` - Close connection
- `httpd_ws_get_connection_count(handle)` - Get connection count

### Channels (Pub/Sub)
- `httpd_ws_join(ws, channel)` - Subscribe to channel
- `httpd_ws_leave(ws, channel)` - Unsubscribe
- `httpd_ws_publish(handle, channel, data, len, type)` - Publish
- `httpd_ws_channel_size(handle, channel)` - Get subscriber count

### Authentication
- `httpd_check_basic_auth(req, user, pass)` - Verify credentials
- `httpd_resp_send_auth_challenge(req, realm)` - Send 401

Kill any qemu-system-xtensa processes forcefully and check the port is open before running tests under QEMU.
- Don't blame QEMU's networking for slow requests, it's never qemu