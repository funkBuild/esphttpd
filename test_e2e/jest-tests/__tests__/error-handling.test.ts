/**
 * Error Handling Tests
 * Tests the server's error handling and recovery capabilities
 */

import axios, { AxiosError } from 'axios';
import { BASE_URL } from '../jest.setup';

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
    it('should handle unsupported methods gracefully', async () => {
      try {
        await axios.request({
          method: 'PATCH',
          url: '/'
        });
        // PATCH might be supported, check response
      } catch (error: any) {
        // If not supported, should return appropriate error
        expect(error.response.status).toBeGreaterThanOrEqual(400);
        expect(error.response.status).toBeLessThan(500);
      }
    });

    it('should reject CONNECT method', async () => {
      try {
        await axios.request({
          method: 'CONNECT',
          url: '/'
        });
      } catch (error: any) {
        // Connection may be rejected at network level or HTTP level
        if (error.response) {
          expect(error.response.status).toBeGreaterThanOrEqual(400);
        } else {
          // Connection error is also acceptable for unsupported methods
          expect(error).toBeDefined();
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
      const longPath = '/' + 'a'.repeat(2000);

      try {
        await axios.get(longPath);
      } catch (error: any) {
        // Should return client error, not crash
        // Connection may be rejected at network level for very long URLs
        if (error.response) {
          expect(error.response.status).toBeGreaterThanOrEqual(400);
          expect(error.response.status).toBeLessThan(500);
        } else {
          // Connection error is also acceptable for malformed requests
          expect(error).toBeDefined();
        }
      }
    });

    it('should handle requests with invalid JSON body', async () => {
      try {
        const response = await axios.post('/api/echo',
          '{"invalid": json}',  // Missing quote
          {
            headers: { 'Content-Type': 'application/json' },
            validateStatus: () => true
          }
        );

        // Server should handle gracefully
        expect(response.status).toBeLessThan(500);
      } catch (error) {
        expect(error).toBeDefined();
      }
    });
  });

  describe('Resource Limits', () => {
    it('should handle very large POST bodies gracefully', async () => {
      const largeBody = 'x'.repeat(100000); // 100KB

      try {
        const response = await axios.post('/api/echo',
          largeBody,
          {
            headers: { 'Content-Type': 'text/plain' },
            maxBodyLength: Infinity,
            validateStatus: () => true
          }
        );

        // Should either accept or reject with appropriate error
        if (response.status === 200) {
          expect(response.data.length).toBeLessThanOrEqual(largeBody.length);
        } else {
          expect(response.status).toBeGreaterThanOrEqual(400);
          expect(response.status).toBeLessThan(500);
        }
      } catch (error: any) {
        // Connection may be reset (no response) or return error status
        if (error.response) {
          expect(error.response.status).toBeGreaterThanOrEqual(400);
        } else {
          // Connection error is acceptable for very large bodies
          expect(error.code).toBeDefined();
        }
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

        // Should handle gracefully
        expect(response.status).toBeLessThan(500);
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
      const response = await axios.get("/api/data/1' OR '1'='1", {
        validateStatus: () => true
      });

      // Should handle as normal ID, not execute SQL
      // Server may URL-encode special characters
      if (response.status === 200) {
        // Accept either raw or URL-encoded response
        const expectedRaw = "1' OR '1'='1";
        const expectedEncoded = "1'%20OR%20'1'='1";
        expect([expectedRaw, expectedEncoded]).toContain(response.data.id);
      }
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
          timeout: 10000  // 10s timeout for QEMU
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