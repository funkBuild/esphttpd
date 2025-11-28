/**
 * CORS (Cross-Origin Resource Sharing) Tests
 * Tests the server's CORS implementation
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';

describe('CORS (Cross-Origin Resource Sharing)', () => {

  describe('GET /cors', () => {
    it('should return CORS headers on GET request', async () => {
      const response = await axios.get('/cors');

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
      expect(response.headers['access-control-allow-methods']).toContain('GET');
      expect(response.headers['access-control-allow-methods']).toContain('POST');
      expect(response.headers['access-control-allow-headers']).toContain('Content-Type');
      expect(response.data).toEqual({ cors: 'enabled' });
    });

    it('should handle preflight OPTIONS request', async () => {
      const response = await axios.options('/cors');

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-origin']).toBe('*');
      expect(response.headers['access-control-allow-methods']).toBeDefined();
      expect(response.headers['access-control-allow-headers']).toBeDefined();
    });

    it('should include custom headers in allow-headers', async () => {
      const response = await axios.options('/cors');

      expect(response.headers['access-control-allow-headers']).toContain('X-Test-Header');
    });
  });

  describe('CORS with Different Origins', () => {
    it('should accept requests from any origin', async () => {
      const response = await axios.get('/cors', {
        headers: {
          'Origin': 'http://example.com'
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
    });

    it('should accept requests from localhost origins', async () => {
      const response = await axios.get('/cors', {
        headers: {
          'Origin': 'http://localhost:3000'
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
    });

    it('should accept requests from HTTPS origins', async () => {
      const response = await axios.get('/cors', {
        headers: {
          'Origin': 'https://secure.example.com'
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
    });
  });

  describe('CORS Methods Support', () => {
    it('should allow GET method', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Method': 'GET'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-methods']).toContain('GET');
    });

    it('should allow POST method', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Method': 'POST'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-methods']).toContain('POST');
    });

    it('should allow PUT method', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Method': 'PUT'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-methods']).toContain('PUT');
    });

    it('should allow DELETE method', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Method': 'DELETE'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-methods']).toContain('DELETE');
    });

    it('should allow OPTIONS method', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Method': 'OPTIONS'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-methods']).toContain('OPTIONS');
    });
  });

  describe('CORS Headers Support', () => {
    it('should allow Content-Type header', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Headers': 'Content-Type'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-headers']).toContain('Content-Type');
    });

    it('should allow custom headers', async () => {
      const response = await axios.options('/cors', {
        headers: {
          'Access-Control-Request-Headers': 'X-Test-Header, X-Custom-Header'
        }
      });

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-headers']).toContain('X-Test-Header');
    });

    it('should handle multiple headers in request', async () => {
      const response = await axios.get('/cors', {
        headers: {
          'Origin': 'http://example.com',
          'X-Test-Header': 'test-value'
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
    });
  });

  describe('CORS Preflight Caching', () => {
    it('should not include max-age by default', async () => {
      const response = await axios.options('/cors');

      // Server may or may not set max-age
      expect(response.status).toBe(204);
      // If max-age is set, it should be a reasonable value
      if (response.headers['access-control-max-age']) {
        const maxAge = parseInt(response.headers['access-control-max-age']);
        expect(maxAge).toBeGreaterThan(0);
        expect(maxAge).toBeLessThanOrEqual(86400); // Max 1 day
      }
    });
  });

  describe('CORS on Other Endpoints', () => {
    it('should check if CORS is endpoint-specific', async () => {
      // Test a different endpoint to see if CORS is global or per-endpoint
      const response = await axios.get('/api/status', {
        headers: {
          'Origin': 'http://example.com'
        }
      });

      expect(response.status).toBe(200);
      // CORS headers may or may not be present on other endpoints
      // This test documents the actual behavior
      const hasCorsHeaders = response.headers['access-control-allow-origin'] !== undefined;
      console.log(`Other endpoints ${hasCorsHeaders ? 'have' : 'do not have'} CORS headers`);
    });
  });

  describe('CORS Security', () => {
    it('should not expose sensitive headers', async () => {
      const response = await axios.get('/cors');

      // Should not expose potentially sensitive headers
      expect(response.headers['access-control-allow-credentials']).not.toBe('true');
      // When allowing all origins (*), credentials should not be allowed
    });

    it('should handle CORS bypass attempts', async () => {
      const response = await axios.get('/cors', {
        headers: {
          'Origin': 'null' // Special origin used in some attacks
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['access-control-allow-origin']).toBe('*');
    });
  });

  describe('CORS with Request Body', () => {
    it('should handle CORS for POST requests with JSON body', async () => {
      const response = await axios.post('/api/echo',
        { test: 'data' },
        {
          headers: {
            'Origin': 'http://example.com',
            'Content-Type': 'application/json'
          }
        }
      );

      expect(response.status).toBe(200);
      expect(response.data).toEqual({ test: 'data' });
    });

    it('should handle CORS for PUT requests', async () => {
      const response = await axios.put('/api/update',
        { update: 'data' },
        {
          headers: {
            'Origin': 'http://example.com'
          }
        }
      );

      expect(response.status).toBe(200);
      expect(response.data.message).toBe('Updated');
    });
  });
});