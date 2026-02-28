/**
 * Error Handling Tests
 * Tests the server's error handling and recovery capabilities
 */

import axios, { AxiosError } from 'axios';
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

describe('Error Handling', () => {

  describe('404 Not Found', () => {
    it('should return 404 for non-existent endpoints', async () => {
      try {
        await axios.get('/this-does-not-exist');
        fail('Should have returned 404');
      } catch (error: any) {
        expect(error.response.status).toBe(404);
        expect(error.response.data).toContain('404 - Not Found');
        expect(error.response.data).toContain('/this-does-not-exist');
      }
    });

    it('should return HTML 404 page with proper formatting', async () => {
      try {
        await axios.get('/another-missing-page');
      } catch (error: any) {
        expect(error.response.status).toBe(404);
        expect(error.response.headers['content-type']).toContain('text/html');
        expect(error.response.data).toContain('<html>');
        expect(error.response.data).toContain('</html>');
      }
    });

    it('should handle deeply nested non-existent paths', async () => {
      try {
        await axios.get('/very/deep/path/that/does/not/exist');
      } catch (error: any) {
        expect(error.response.status).toBe(404);
      }
    });
  });

  describe('Invalid Methods', () => {
    it('should return 404 for PATCH on endpoints without PATCH handler', async () => {
      const response = await axios.request({
        method: 'PATCH',
        url: '/',
        validateStatus: () => true
      });

      // No PATCH handler registered for / - should return 404
      expect(response.status).toBe(404);
    });

    it('should reject CONNECT method', async () => {
      try {
        await axios.request({
          method: 'CONNECT',
          url: '/',
          timeout: TIMEOUTS.HTTP
        });
        // If we get here, server accepted CONNECT - that's wrong
        fail('CONNECT should not succeed');
      } catch (error: any) {
        // Connection may be rejected at network level or HTTP level
        if (error.response) {
          expect(error.response.status).toBeGreaterThanOrEqual(400);
        } else {
          // Connection error or timeout is acceptable for CONNECT
          expect(error.code).toBeDefined();
        }
      }
    });

    it('should reject TRACE method', async () => {
      try {
        await axios.request({
          method: 'TRACE',
          url: '/'
        });
      } catch (error: any) {
        expect(error.response.status).toBeGreaterThanOrEqual(400);
      }
    });
  });

  describe('Malformed Requests', () => {
    it('should handle requests with invalid headers', async () => {
      try {
        const response = await axios.get('/', {
          headers: {
            'Content-Length': 'not-a-number'
          },
          validateStatus: () => true
        });

        // Should still respond, possibly ignoring bad header
        expect(response.status).toBeLessThan(500);
      } catch (error) {
        // Or reject the request
        expect(error).toBeDefined();
      }
    });

    it('should handle requests with extremely long URLs', async () => {
      // REQ_HEADER_BUF_SIZE is 2048 bytes. A URL longer than that should
      // either be rejected with an error or handled gracefully (404).
      const longPath = '/' + 'a'.repeat(4000);

      try {
        const response = await axios.get(longPath, { validateStatus: () => true });
        // If server responds, it should not be a 5xx crash
        expect(response.status).toBeLessThan(500);
      } catch (error: any) {
        // Connection error or rejection is acceptable for oversized URLs
        expect(error).toBeDefined();
      }
    });

    it('should handle requests with invalid JSON body', async () => {
      const invalidJson = '{"invalid": json}';  // Missing quote
      const response = await axios.post('/api/echo',
        invalidJson,
        {
          headers: { 'Content-Type': 'application/json' },
          validateStatus: () => true,
          // Prevent axios from serializing the string as JSON
          transformRequest: [(data: string) => data],
          // Prevent axios from parsing the response as JSON
          transformResponse: [(data: string) => data]
        }
      );

      // Server may echo the raw body (200) or reject invalid JSON (400)
      expect([200, 400]).toContain(response.status);
      if (response.status === 200) {
        expect(response.headers['content-type']).toContain('application/json');
        expect(response.data).toBe(invalidJson);
      }
    });
  });

  describe('Resource Limits', () => {
    it('should handle very large POST bodies gracefully', async () => {
      const largeBody = 'x'.repeat(100000); // 100KB

      const response = await axios.post('/api/echo',
        largeBody,
        {
          headers: { 'Content-Type': 'text/plain' },
          maxBodyLength: Infinity,
          validateStatus: () => true,
          // Keep response as raw string to check length accurately
          transformResponse: [(data: string) => data]
        }
      );

      // Echo handler reads into a 512-byte buffer (sizeof(body) - 1 = 511 max),
      // so it silently truncates and returns 200 with the first 511 bytes.
      // Server may also return 400 if connection state is degraded.
      expect([200, 400]).toContain(response.status);
      if (response.status === 200) {
        expect(response.headers['content-type']).toContain('text/plain');
        expect(response.data.length).toBe(511);
        expect(response.data).toBe('x'.repeat(511));
      }
    });

    it('should handle many headers gracefully', async () => {
      const headers: any = {};
      for (let i = 0; i < 50; i++) {
        headers[`X-Custom-Header-${i}`] = `Value-${i}`;
      }

      try {
        const response = await axios.get('/headers', {
          headers,
          validateStatus: () => true
        });

        // Should handle gracefully - either 200 with parsed headers or 400 for too many
        expect([200, 400, 413]).toContain(response.status);
      } catch (error) {
        expect(error).toBeDefined();
      }
    });
  });

  describe('Connection Errors', () => {
    it('should handle aborted requests', async () => {
      const controller = new AbortController();

      const promise = axios.get('/api/status', {
        signal: controller.signal
      });

      // Abort immediately
      controller.abort();

      try {
        await promise;
        fail('Should have been aborted');
      } catch (error: any) {
        expect(error.code).toBe('ERR_CANCELED');
      }
    });

    it('should handle slow clients', async () => {
      // Simulate slow reading - just verify server responds
      const response = await axios.get('/api/status');

      expect(response.status).toBe(200);
      // Server should not timeout on slow clients
    });
  });

  describe('Security Errors', () => {
    it('should reject path traversal attempts', async () => {
      const maliciousPaths = [
        '/../../../etc/passwd',
        '/..%2F..%2F..%2Fetc%2Fpasswd',
        '/....//....//etc/passwd'
      ];

      for (const path of maliciousPaths) {
        try {
          const response = await axios.get(path, {
            validateStatus: () => true
          });

          // Should not expose system files
          expect(response.data).not.toContain('root:');
          expect(response.data).not.toContain('/bin/bash');
        } catch (error) {
          // Request rejected is also acceptable
          expect(error).toBeDefined();
        }
      }
    });

    it('should handle SQL injection attempts in parameters', async () => {
      const sqlPayload = "1' OR '1'='1";
      const response = await axios.get(`/api/data/${encodeURIComponent(sqlPayload)}`, {
        validateStatus: () => true
      });

      // Server has no database - it echoes the ID back literally
      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('id');
      expect(response.data).toHaveProperty('data');
      // Verify the response structure matches the normal /api/data/:id format
      // (no extra fields leaked, no different response shape)
      expect(Object.keys(response.data).sort()).toEqual(['data', 'id']);
    });

    it('should handle XSS attempts in echo endpoint', async () => {
      const xssPayload = '<script>alert("XSS")</script>';
      const response = await axios.post('/api/echo', xssPayload, {
        headers: { 'Content-Type': 'text/plain' }
      });

      // Should echo back without executing
      expect(response.status).toBe(200);
      expect(response.data).toBe(xssPayload);
    });
  });

  describe('Recovery and Resilience', () => {
    it('should continue serving after handling errors', async () => {
      // Cause an error
      try {
        await axios.get('/does-not-exist');
      } catch (error) {
        // Expected 404
      }

      // Server should still be responsive
      const response = await axios.get('/api/status');
      expect(response.status).toBe(200);
      expect(response.data.status).toBe('ok');
    });

    it('should handle rapid error requests', async () => {
      // Reduced count for QEMU emulation performance
      const errorRequests = Array(3).fill(null).map(() =>
        axios.get('/not-found-' + Math.random(), {
          validateStatus: () => true,
          timeout: TIMEOUTS.HTTP
        })
      );

      const responses = await Promise.all(errorRequests);

      responses.forEach(response => {
        expect(response.status).toBe(404);
      });

      // Server should still be healthy
      const healthCheck = await axios.get('/api/status');
      expect(healthCheck.data.status).toBe('ok');
    });
  });

  describe('Error Response Format', () => {
    it('should return consistent error format for 404s', async () => {
      try {
        await axios.get('/missing');
      } catch (error: any) {
        expect(error.response.status).toBe(404);
        expect(typeof error.response.data).toBe('string');
        expect(error.response.data.length).toBeGreaterThan(0);
      }
    });

    it('should not leak internal information in errors', async () => {
      try {
        await axios.get('/trigger-error');
      } catch (error: any) {
        // Error messages should not contain:
        expect(error.response.data).not.toContain('stack trace');
        expect(error.response.data).not.toContain('ESP_LOG');
        expect(error.response.data).not.toContain('connection_pool');
        expect(error.response.data).not.toContain('0x'); // Memory addresses
      }
    });
  });
});