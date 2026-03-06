#ifndef _RAW_TCP_H_
#define _RAW_TCP_H_

#include "sdkconfig.h"

#ifdef CONFIG_HTTPD_USE_RAW_API

#include <stdint.h>
#include <stdbool.h>
#include "connection.h"
#include "event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

// Poll interval in half-seconds (configurable via Kconfig)
#ifndef CONFIG_HTTPD_RAW_POLL_INTERVAL
#define CONFIG_HTTPD_RAW_POLL_INTERVAL 4
#endif

// Create listening PCB and start accepting connections
// Must be called from tcpip_thread context (or under LOCK_TCPIP_CORE)
// Returns 0 on success, -1 on error
int raw_tcp_listen(event_loop_t* loop, const event_handlers_t* handlers);

// Write data to a raw TCP connection
// Returns bytes written, or -1 on error
// Respects tcp_sndbuf() limits; caller must handle partial writes
ssize_t raw_tcp_write(connection_t* conn, const void* data, size_t len, bool more);

// Get available send buffer space for a connection
size_t raw_tcp_sndbuf(connection_t* conn);

// Flush pending tcp_write data (calls tcp_output)
void raw_tcp_output(connection_t* conn);

// Close a raw TCP connection
// Clears all callbacks, closes PCB
void raw_tcp_close(connection_t* conn);

// Close the listening PCB and all active connections
void raw_tcp_stop(event_loop_t* loop);

// Acknowledge received data (call after processing)
void raw_tcp_recved(connection_t* conn, uint16_t len);

#ifdef CONFIG_ESPHTTPD_TEST_MODE
// Test mock: capture output instead of calling tcp_write
typedef ssize_t (*raw_tcp_write_mock_t)(connection_t* conn, const void* data, size_t len, bool more);
void raw_tcp_set_write_mock(raw_tcp_write_mock_t mock);
#endif

#ifdef __cplusplus
}
#endif

#endif // CONFIG_HTTPD_USE_RAW_API
#endif // _RAW_TCP_H_
