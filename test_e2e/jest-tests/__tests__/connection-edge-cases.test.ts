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
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

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
      // Server should return 200 with empty or minimal body
      expect(response.data.length).toBeLessThanOrEqual(1);
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
      const response1 = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(response1.status).toBe(200);
      expect(response1.data).toHaveProperty('status', 'ok');

      const response2 = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(response2.status).toBe(200);
      expect(response2.data).toHaveProperty('status', 'ok');
    });

    it('should handle sequential requests to different endpoints', async () => {
      const response1 = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(response1.status).toBe(200);
      expect(response1.data).toHaveProperty('status');

      const response2 = await axios.get('/hello', { timeout: TIMEOUTS.HTTP });
      expect(response2.status).toBe(200);
      expect(response2.data).toHaveProperty('message', 'Hello, World!');

      const response3 = await axios.get('/api/data/seq-test', { timeout: TIMEOUTS.HTTP });
      expect(response3.status).toBe(200);
      expect(response3.data).toHaveProperty('id', 'seq-test');
    });

    it('should handle mixed GET and POST sequential requests', async () => {
      const getResponse = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(getResponse.status).toBe(200);

      const postResponse = await axios.post('/api/echo',
        { test: 'sequential' },
        { timeout: TIMEOUTS.HTTP }
      );
      expect(postResponse.status).toBe(200);
      expect(postResponse.data).toEqual({ test: 'sequential' });

      // Verify server is still responsive after the sequence
      const finalResponse = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(finalResponse.status).toBe(200);
    });

    it('should handle rapid sequential requests without errors', async () => {
      // Send 5 sequential requests as quickly as possible
      for (let i = 0; i < 5; i++) {
        const response = await axios.get(`/api/data/rapid-${i}`, { timeout: TIMEOUTS.HTTP });
        expect(response.status).toBe(200);
        expect(response.data.id).toBe(`rapid-${i}`);
      }
    });
  });

  describe('Connection pool recovery', () => {
    it('should handle 5 concurrent requests successfully', async () => {
      const requests = Array(5).fill(null).map((_, i) =>
        axios.get('/api/status', { timeout: TIMEOUTS.HTTP })
      );

      const responses = await Promise.all(requests);

      responses.forEach((response) => {
        expect(response.status).toBe(200);
        expect(response.data).toHaveProperty('status', 'ok');
      });
    }, TIMEOUTS.CONCURRENT);

    it('should still respond after concurrent burst', async () => {
      // First, send a burst of concurrent requests
      const burstRequests = Array(5).fill(null).map((_, i) =>
        axios.get(`/api/data/burst-${i}`, { timeout: TIMEOUTS.HTTP })
      );

      const burstResponses = await Promise.all(burstRequests);
      burstResponses.forEach((response, i) => {
        expect(response.status).toBe(200);
        expect(response.data.id).toBe(`burst-${i}`);
      });

      // Then verify the server is still healthy
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(healthCheck.status).toBe(200);
      expect(healthCheck.data.status).toBe('ok');
    }, TIMEOUTS.CONCURRENT);

    it('should handle mixed concurrent and sequential requests', async () => {
      // Concurrent batch
      const concurrent = await Promise.all([
        axios.get('/api/status', { timeout: TIMEOUTS.HTTP }),
        axios.get('/hello', { timeout: TIMEOUTS.HTTP }),
        axios.get('/api/data/mix-1', { timeout: TIMEOUTS.HTTP })
      ]);

      concurrent.forEach((response) => {
        expect(response.status).toBe(200);
      });

      // Sequential follow-up
      const seq1 = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(seq1.status).toBe(200);

      // Another concurrent batch
      const concurrent2 = await Promise.all([
        axios.get('/api/data/mix-2', { timeout: TIMEOUTS.HTTP }),
        axios.get('/api/data/mix-3', { timeout: TIMEOUTS.HTTP })
      ]);

      concurrent2.forEach((response) => {
        expect(response.status).toBe(200);
      });
    }, TIMEOUTS.CONCURRENT);

    it('should recover after error responses in concurrent requests', async () => {
      // Mix valid and invalid requests concurrently
      const requests = [
        axios.get('/api/status', { timeout: TIMEOUTS.HTTP, validateStatus: () => true }),
        axios.get('/nonexistent-1', { timeout: TIMEOUTS.HTTP, validateStatus: () => true }),
        axios.get('/api/data/valid', { timeout: TIMEOUTS.HTTP, validateStatus: () => true }),
        axios.get('/nonexistent-2', { timeout: TIMEOUTS.HTTP, validateStatus: () => true }),
        axios.get('/hello', { timeout: TIMEOUTS.HTTP, validateStatus: () => true })
      ];

      const responses = await Promise.all(requests);

      // Valid endpoints should return 200
      expect(responses[0].status).toBe(200);
      expect(responses[2].status).toBe(200);
      expect(responses[4].status).toBe(200);

      // Invalid endpoints should return 404
      expect(responses[1].status).toBe(404);
      expect(responses[3].status).toBe(404);

      // Server should still be healthy after the mixed batch
      const healthCheck = await axios.get('/api/status', { timeout: TIMEOUTS.HTTP });
      expect(healthCheck.status).toBe(200);
      expect(healthCheck.data.status).toBe('ok');
    }, TIMEOUTS.CONCURRENT);
  });
});
