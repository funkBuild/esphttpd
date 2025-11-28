/**
 * HTTP Routes Tests
 * Tests all basic HTTP route functionality
 */

import axios, { AxiosResponse } from 'axios';
import { BASE_URL } from '../jest.setup';

describe('HTTP Routes', () => {

  describe('GET /', () => {
    it('should return home page with correct content', async () => {
      const response = await axios.get('/');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/html');
      expect(response.data).toContain('ESP32 E2E Test Server');
      expect(response.data).toContain('Server is running!');
      expect(response.data).toContain('Request count:');
    });
  });

  describe('GET /api/status', () => {
    it('should return server status as JSON', async () => {
      const response = await axios.get('/api/status');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toHaveProperty('status', 'ok');
      expect(response.data).toHaveProperty('uptime');
      expect(response.data).toHaveProperty('requests');
      expect(response.data).toHaveProperty('ws_connections');
      expect(typeof response.data.uptime).toBe('number');
      expect(typeof response.data.requests).toBe('number');
    });
  });

  describe('GET /api/data/:id', () => {
    it('should return data for specific ID', async () => {
      const testId = '12345';
      const response = await axios.get(`/api/data/${testId}`);

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toHaveProperty('id', testId);
      expect(response.data).toHaveProperty('data', `Test data for ${testId}`);
    });

    it('should handle special characters in ID', async () => {
      const testId = 'test-id_123';
      const response = await axios.get(`/api/data/${testId}`);

      expect(response.status).toBe(200);
      expect(response.data.id).toBe(testId);
    });
  });

  describe('POST /api/echo', () => {
    it('should echo back JSON data', async () => {
      const testData = {
        message: 'Hello ESP32',
        number: 42,
        nested: { key: 'value' }
      };

      const response = await axios.post('/api/echo', testData);

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toEqual(testData);
    });

    it('should handle empty POST body', async () => {
      const response = await axios.post('/api/echo', {});

      expect(response.status).toBe(200);
      expect(response.data).toEqual({});
    });

    it('should handle plain text POST', async () => {
      const testData = 'Plain text data';
      const response = await axios.post('/api/echo', testData, {
        headers: { 'Content-Type': 'text/plain' }
      });

      expect(response.status).toBe(200);
      expect(response.data).toBe(testData);
    });
  });

  describe('PUT /api/update', () => {
    it('should accept update data and return confirmation', async () => {
      const updateData = { field: 'value', update: true };
      const response = await axios.put('/api/update', updateData);

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toHaveProperty('message', 'Updated');
      expect(response.data).toHaveProperty('bytes_received');
      expect(response.data.bytes_received).toBeGreaterThan(0);
    });
  });

  describe('DELETE /api/data/:id', () => {
    it('should delete data by ID', async () => {
      const testId = 'delete-me-999';
      const response = await axios.delete(`/api/data/${testId}`);

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toHaveProperty('message', 'Deleted');
      expect(response.data).toHaveProperty('id', testId);
    });
  });

  describe('Request Methods', () => {
    it('should handle HEAD requests', async () => {
      const response = await axios.head('/');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/html');
      expect(response.data).toBe(''); // HEAD should not return body
    });

    it('should handle OPTIONS requests for CORS', async () => {
      const response = await axios.options('/cors');

      expect(response.status).toBe(204);
      expect(response.headers['access-control-allow-origin']).toBe('*');
      expect(response.headers['access-control-allow-methods']).toContain('GET');
      expect(response.headers['access-control-allow-methods']).toContain('POST');
    });
  });

  describe('Request Headers', () => {
    it('should accept and process custom headers', async () => {
      const customHeaderValue = 'TestValue123';
      const response = await axios.get('/headers', {
        headers: {
          'X-Test-Header': customHeaderValue
        }
      });

      expect(response.status).toBe(200);
      // Server returns JSON with header values
      expect(response.data['X-Test-Header']).toBe(customHeaderValue);
    });

    it('should handle User-Agent header', async () => {
      const response = await axios.get('/headers', {
        headers: {
          'User-Agent': 'Jest Test Suite',
          'X-Test-Header': 'user-agent-test'
        }
      });

      // Server returns JSON with stored headers
      expect(response.data['X-Test-Header']).toBe('user-agent-test');
      // Host header should match the server URL being tested
      expect(response.data['Host']).toMatch(/^127\.0\.0\.1:\d+$/);
    });
  });

  describe('Content Types', () => {
    it('should handle different Accept headers', async () => {
      const response = await axios.get('/api/status', {
        headers: {
          'Accept': 'application/json'
        }
      });

      expect(response.headers['content-type']).toContain('application/json');
    });
  });

  describe('Performance', () => {
    it('should respond within reasonable time', async () => {
      const startTime = Date.now();
      await axios.get('/api/status');
      const responseTime = Date.now() - startTime;

      expect(responseTime).toBeLessThan(3000); // QEMU: Increased from 500ms to 3s
    });

    it('should handle multiple concurrent requests', async () => {
      // Use fewer concurrent requests for QEMU emulation
      const requests = Array(3).fill(null).map((_, i) =>
        axios.get(`/api/data/concurrent-${i}`, { timeout: 15000 })
      );

      const responses = await Promise.all(requests);

      responses.forEach((response, i) => {
        expect(response.status).toBe(200);
        expect(response.data.id).toBe(`concurrent-${i}`);
      });
    }, 25000); // QEMU: Extended timeout for concurrent requests
  });
});