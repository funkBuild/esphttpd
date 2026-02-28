/**
 * Connection Edge Cases Tests
 *
 * Tests HTTP connection behavior including:
 * - Connection: close header handling
 * - Zero-length body handling
 * - Duplicate/multiple custom headers
 * - Sequential requests on keep-alive connections
 * - Connection pool recovery under concurrent load
 */

import axios from 'axios';
import * as http from 'http';
import * as net from 'net';
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

/** Send a raw HTTP request and return the response as a string. */
function rawRequest(host: string, port: number, request: string, timeoutMs = TIMEOUTS.HTTP): Promise<string> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port }, () => {
      socket.write(request);
    });

    let data = '';
    socket.on('data', (chunk) => { data += chunk.toString(); });
    socket.on('end', () => resolve(data));
    socket.on('error', (err) => reject(err));

    const timer = setTimeout(() => {
      socket.destroy();
      // Treat timeout as successful if we got partial data
      if (data.length > 0) resolve(data);
      else reject(new Error('Raw request timed out'));
    }, timeoutMs);

    socket.on('close', () => clearTimeout(timer));
  });
}

describe('Connection Edge Cases', () => {

  describe('Connection: close behavior', () => {
    it('should respond and close connection when Connection: close is sent', async () => {
      const response = await axios.get('/api/status', {
        headers: {
          'Connection': 'close'
        },
        timeout: TIMEOUTS.HTTP
      });

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('status', 'ok');
    });

    it('should include Connection: close in response headers', async () => {
      const response = await axios.get('/api/status', {
        headers: {
          'Connection': 'close'
        },
        timeout: TIMEOUTS.HTTP
      });

      expect(response.status).toBe(200);
      // Server should echo back Connection: close to confirm it will close
      expect(response.headers['connection']).toBe('close');
    });

    it('should close the TCP connection after responding', (done) => {
      // Use raw http.get to observe the socket close event directly
      const url = new URL('/api/status', BASE_URL);

      const req = http.get(
        {
          hostname: url.hostname,
          port: url.port,
          path: url.pathname,
          headers: { 'Connection': 'close' }
        },
        (res) => {
          expect(res.statusCode).toBe(200);

          let body = '';
          res.on('data', (chunk: Buffer) => { body += chunk.toString(); });
          res.on('end', () => {
            // Verify we got a valid response
            const data = JSON.parse(body);
            expect(data.status).toBe('ok');
          });

          // The socket should be closed by the server
          res.on('close', () => {
            done();
          });
        }
      );

      req.setTimeout(15000, () => {
        req.destroy();
        done(new Error('Request timed out'));
      });

      req.on('error', (err) => {
        done(err);
      });
    }, TIMEOUTS.HTTP + 5000);
  });

  describe('Zero-length body', () => {
    it('should handle POST with empty string body and Content-Length: 0', async () => {
      const response = await axios.post('/api/echo', '', {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Length': '0'
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true,
        // Prevent axios from transforming the empty string
        transformRequest: [(data: string) => data],
        transformResponse: [(data: string) => data]
      });

      expect(response.status).toBe(200);
      // Server echoes back whatever it received - empty body yields empty response
      expect(typeof response.data).toBe('string');
      expect(response.data).toBe('');
    });

    it('should handle POST with no body at all', async () => {
      const response = await axios.post('/api/echo', undefined, {
        headers: {
          'Content-Type': 'text/plain'
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true,
        transformResponse: [(data: string) => data]
      });

      expect(response.status).toBe(200);
    });

    it('should handle PUT with empty body', async () => {
      const response = await axios.put('/api/update', '', {
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': '0'
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      // Server should accept the request without crashing
      expect(response.status).toBeLessThan(500);
    });
  });

  describe('Custom headers', () => {
    it('should echo X-Test-Header back in response', async () => {
      const response = await axios.get('/headers', {
        headers: {
          'X-Test-Header': 'hello-world'
        },
        timeout: TIMEOUTS.HTTP
      });

      expect(response.status).toBe(200);
      // The /headers endpoint echoes Host and X-Test-Header
      expect(response.data).toHaveProperty('Host');
      expect(response.data['X-Test-Header']).toBe('hello-world');
    });

    it('should handle request with many extra headers without crashing', async () => {
      const headers: Record<string, string> = {
        'X-Test-Header': 'preserved-value'
      };
      // Add many extra headers the server won't echo but should tolerate
      for (let i = 1; i <= 10; i++) {
        headers[`X-Extra-${i}`] = `Value-${i}`;
      }

      const response = await axios.get('/headers', {
        headers,
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      expect(response.status).toBe(200);
      // Server should still echo the known header correctly
      expect(response.data['X-Test-Header']).toBe('preserved-value');
    });

    it('should handle headers with varying value lengths', async () => {
      // Send X-Test-Header with a long value - server should still parse it
      const longValue = 'a'.repeat(200);
      const response = await axios.get('/headers', {
        headers: {
          'X-Test-Header': longValue
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      expect(response.status).toBe(200);
      expect(response.data['X-Test-Header']).toBe(longValue);
    });
  });

  describe('Sequential requests on same connection (keep-alive)', () => {
    it('should handle two sequential requests successfully', async () => {
      // By default HTTP/1.1 uses keep-alive, and axios reuses connections
      // via its underlying http agent
      const response1 = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(response1.status).toBe(200);
      expect(response1.data).toHaveProperty('status', 'ok');

      const response2 = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(response2.status).toBe(200);
      expect(response2.data).toHaveProperty('status', 'ok');
    });

    it('should handle sequential requests to different endpoints', async () => {
      const response1 = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(response1.status).toBe(200);
      expect(response1.data).toHaveProperty('status');

      const response2 = await axios.get('/hello', { timeout: TIMEOUTS.CONCURRENT });
      expect(response2.status).toBe(200);
      expect(response2.data).toHaveProperty('message', 'Hello, World!');

      const response3 = await axios.get('/api/data/seq-test', { timeout: TIMEOUTS.CONCURRENT });
      expect(response3.status).toBe(200);
      expect(response3.data).toHaveProperty('id', 'seq-test');
    });

    it('should handle mixed GET and POST sequential requests', async () => {
      const getResponse = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(getResponse.status).toBe(200);

      const postResponse = await axios.post('/api/echo',
        { test: 'sequential' },
        { timeout: TIMEOUTS.CONCURRENT }
      );
      expect(postResponse.status).toBe(200);
      expect(postResponse.data).toEqual({ test: 'sequential' });

      // Verify server is still responsive after the sequence
      const finalResponse = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(finalResponse.status).toBe(200);
    });

    it('should handle rapid sequential requests without errors', async () => {
      // Send 5 sequential requests as quickly as possible
      for (let i = 0; i < 5; i++) {
        const response = await axios.get(`/api/data/rapid-${i}`, { timeout: TIMEOUTS.CONCURRENT });
        expect(response.status).toBe(200);
        expect(response.data.id).toBe(`rapid-${i}`);
      }
    });
  });

  describe('Connection pool recovery', () => {
    it('should handle 5 concurrent requests successfully', async () => {
      const requests = Array(5).fill(null).map((_, i) =>
        axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT })
      );

      const responses = await Promise.all(requests);

      responses.forEach((response) => {
        expect(response.status).toBe(200);
        expect(response.data).toHaveProperty('status', 'ok');
      });
    }, TIMEOUTS.CONCURRENT + 5000);

    it('should still respond after concurrent burst', async () => {
      // Send a burst of concurrent requests (3 for QEMU reliability)
      const burstRequests = Array(3).fill(null).map((_, i) =>
        axios.get(`/api/data/burst-${i}`, { timeout: TIMEOUTS.CONCURRENT })
      );

      const burstResponses = await Promise.all(burstRequests);
      burstResponses.forEach((response, i) => {
        expect(response.status).toBe(200);
        expect(response.data.id).toBe(`burst-${i}`);
      });

      // Then verify the server is still healthy
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(healthCheck.status).toBe(200);
      expect(healthCheck.data.status).toBe('ok');
    }, TIMEOUTS.CONCURRENT + 10000);

    it('should handle mixed concurrent and sequential requests', async () => {
      // Concurrent batch
      const concurrent = await Promise.all([
        axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT }),
        axios.get('/hello', { timeout: TIMEOUTS.CONCURRENT }),
        axios.get('/api/data/mix-1', { timeout: TIMEOUTS.CONCURRENT })
      ]);

      concurrent.forEach((response) => {
        expect(response.status).toBe(200);
      });

      // Sequential follow-up
      const seq1 = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(seq1.status).toBe(200);

      // Another concurrent batch
      const concurrent2 = await Promise.all([
        axios.get('/api/data/mix-2', { timeout: TIMEOUTS.CONCURRENT }),
        axios.get('/api/data/mix-3', { timeout: TIMEOUTS.CONCURRENT })
      ]);

      concurrent2.forEach((response) => {
        expect(response.status).toBe(200);
      });
    }, TIMEOUTS.CONCURRENT + 10000);

    it('should recover after error responses in concurrent requests', async () => {
      // Mix valid and invalid requests concurrently (3 for QEMU reliability)
      const requests = [
        axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT, validateStatus: () => true }),
        axios.get('/nonexistent-1', { timeout: TIMEOUTS.CONCURRENT, validateStatus: () => true }),
        axios.get('/api/data/valid', { timeout: TIMEOUTS.CONCURRENT, validateStatus: () => true })
      ];

      const responses = await Promise.all(requests);

      // Valid endpoints should return 200
      expect(responses[0].status).toBe(200);
      expect(responses[2].status).toBe(200);

      // Invalid endpoint should return 404
      expect(responses[1].status).toBe(404);

      // Server should still be healthy after the mixed batch
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(healthCheck.status).toBe(200);
      expect(healthCheck.data.status).toBe('ok');
    }, TIMEOUTS.CONCURRENT + 10000);
  });

  describe('Malformed HTTP headers', () => {
    const url = new URL(BASE_URL);
    const host = url.hostname;
    const port = parseInt(url.port, 10);

    it('should handle oversized header value without crashing', async () => {
      // REQ_HEADER_BUF_SIZE is 2048 bytes - send a header exceeding that
      const bigValue = 'X'.repeat(3000);
      const request = `GET /api/status HTTP/1.1\r\nHost: ${host}:${port}\r\nX-Big: ${bigValue}\r\nConnection: close\r\n\r\n`;

      try {
        const response = await rawRequest(host, port, request, 2000);
        // Server should respond (possibly with error) without crashing
        expect(response).toContain('HTTP/1.');
        expect(response).not.toContain('500');
      } catch {
        // Connection reset or timeout is acceptable
      }

      // Server should still be healthy
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(healthCheck.status).toBe(200);
    }, TIMEOUTS.HTTP + 5000);

    it('should handle header line without colon separator', async () => {
      const request = `GET /api/status HTTP/1.1\r\nHost: ${host}:${port}\r\nMalformedHeaderNoColon\r\nConnection: close\r\n\r\n`;

      try {
        const response = await rawRequest(host, port, request, 2000);
        expect(response).toContain('HTTP/1.');
      } catch {
        // Connection rejected is acceptable
      }

      // Server should still be healthy
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(healthCheck.status).toBe(200);
    }, TIMEOUTS.HTTP + 5000);

    it('should handle empty header name', async () => {
      const request = `GET /api/status HTTP/1.1\r\nHost: ${host}:${port}\r\n: empty-name\r\nConnection: close\r\n\r\n`;

      try {
        const response = await rawRequest(host, port, request, 2000);
        expect(response).toContain('HTTP/1.');
      } catch {
        // Connection rejected is acceptable
      }

      // Server should still be healthy
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.CONCURRENT });
      expect(healthCheck.status).toBe(200);
    }, TIMEOUTS.HTTP + 5000);
  });
});
