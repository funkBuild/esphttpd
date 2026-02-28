/**
 * Shared test utilities for E2E tests
 */

import WebSocket from 'ws';

/**
 * Centralized timeout constants for QEMU emulation (milliseconds).
 * Adjust these values when switching between QEMU and real hardware.
 */
export const TIMEOUTS = {
  /** Standard HTTP request timeout. Also set as axios default in jest.setup.ts. */
  HTTP: 15000,
  /** Small file download (up to ~100KB) */
  DOWNLOAD_SM: 30000,
  /** Medium file download (~128-256KB) */
  DOWNLOAD_MD: 60000,
  /** Large file download (~512KB) */
  DOWNLOAD_LG: 120000,
  /** Very large file download (~1MB) */
  DOWNLOAD_XL: 180000,
  /** Single WebSocket handshake (~60s observed in QEMU) */
  WS_HANDSHAKE: 90000,
  /** Two sequential WebSocket handshakes */
  WS_DUAL_HANDSHAKE: 180000,
  /** WebSocket message wait (echo, channel ops) */
  WS_MESSAGE: 10000,
  /** WebSocket broadcast / multi-client operations */
  WS_BROADCAST: 20000,
  /** Concurrent HTTP request batches */
  CONCURRENT: 30000,
  /** Performance test setup (warmup + iterations) */
  PERF_SETUP: 180000,
  /** Performance test setup (larger files) */
  PERF_SETUP_LARGE: 360000,
  /** Performance concurrent download test */
  PERF_CONCURRENT: 120000,
};

/**
 * Minimum throughput (KB/s) for performance regression guards.
 * Conservative default for QEMU emulation. Override via MIN_THROUGHPUT_KBPS
 * environment variable for faster environments (e.g. real hardware).
 */
export const MIN_THROUGHPUT_KBPS = parseInt(process.env.MIN_THROUGHPUT_KBPS || '100', 10);

// Test credentials (must match server-side TEST_AUTH_USER/TEST_AUTH_PASS)
export const TEST_USER = 'testuser';
export const TEST_PASS = 'testpass';

/**
 * Verify that a buffer contains the expected 1KB block pattern.
 * Each block starts with "BLOCK_NNNN:" where NNNN is the zero-padded block number.
 */
export function verifyBlockPattern(data: Buffer, expectedBlocks: number): boolean {
  for (let block = 0; block < expectedBlocks; block++) {
    const offset = block * 1024;
    const expectedHeader = `BLOCK_${block.toString().padStart(4, '0')}:`;
    const actualHeader = data.slice(offset, offset + 11).toString();

    if (actualHeader !== expectedHeader) {
      console.error(`Block ${block}: expected "${expectedHeader}", got "${actualHeader}"`);
      return false;
    }
  }
  return true;
}

/**
 * Create a Basic Auth header value from username and password.
 */
export function basicAuthHeader(user: string, pass: string): string {
  const credentials = Buffer.from(`${user}:${pass}`).toString('base64');
  return `Basic ${credentials}`;
}

/**
 * Wait for a WebSocket message matching a filter function.
 * Returns a Promise that resolves with the parsed JSON message.
 */
export function waitForMsg(ws: WebSocket, filter: (msg: any) => boolean, timeoutMs = TIMEOUTS.WS_MESSAGE): Promise<any> {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      ws.removeListener('message', handler);
      reject(new Error('Timeout waiting for matching WebSocket message'));
    }, timeoutMs);

    const handler = (data: any) => {
      try {
        const msg = JSON.parse(data.toString());
        if (filter(msg)) {
          clearTimeout(timeout);
          ws.removeListener('message', handler);
          resolve(msg);
        }
      } catch (e) {
        // Ignore non-JSON messages
      }
    };
    ws.on('message', handler);
  });
}
