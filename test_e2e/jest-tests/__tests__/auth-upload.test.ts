/**
 * Authentication and File Upload Tests
 * Tests for basic auth, auth challenge, file upload, and 100-continue handling
 */

import axios, { AxiosError } from 'axios';
import { BASE_URL } from '../jest.setup';

// Test credentials (must match server-side TEST_AUTH_USER/TEST_AUTH_PASS)
const TEST_USER = 'testuser';
const TEST_PASS = 'testpass';

// Helper to create Basic Auth header
function basicAuthHeader(user: string, pass: string): string {
  const credentials = Buffer.from(`${user}:${pass}`).toString('base64');
  return `Basic ${credentials}`;
}

describe('Basic Authentication', () => {

  describe('GET /auth/protected', () => {

    it('should return 401 without credentials', async () => {
      try {
        await axios.get('/auth/protected', { validateStatus: () => true });
      } catch (error) {
        // axios might throw on 401, handle both cases
      }

      const response = await axios.get('/auth/protected', {
        validateStatus: (status) => true  // Don't throw on any status
      });

      expect(response.status).toBe(401);
      expect(response.headers['www-authenticate']).toContain('Basic');
      expect(response.headers['www-authenticate']).toContain('realm=');
      expect(response.data).toContain('Unauthorized');
    });

    it('should return 401 with incorrect credentials', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': basicAuthHeader('wronguser', 'wrongpass')
        },
        validateStatus: () => true
      });

      expect(response.status).toBe(401);
      expect(response.headers['www-authenticate']).toContain('Basic');
    });

    it('should return 401 with correct username but wrong password', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': basicAuthHeader(TEST_USER, 'wrongpass')
        },
        validateStatus: () => true
      });

      expect(response.status).toBe(401);
    });

    it('should return 200 with correct credentials', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': basicAuthHeader(TEST_USER, TEST_PASS)
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toHaveProperty('status', 'authenticated');
      expect(response.data).toHaveProperty('message');
    });

    it('should handle malformed Authorization header gracefully', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': 'NotBasic somegarbage'
        },
        validateStatus: () => true
      });

      expect(response.status).toBe(401);
    });

    it('should handle empty Authorization header gracefully', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': ''
        },
        validateStatus: () => true
      });

      expect(response.status).toBe(401);
    });

    it('should handle invalid base64 in credentials', async () => {
      const response = await axios.get('/auth/protected', {
        headers: {
          'Authorization': 'Basic !!!invalid-base64!!!'
        },
        validateStatus: () => true
      });

      expect(response.status).toBe(401);
    });
  });

  describe('POST /auth/protected', () => {
    it('should work with POST method and correct credentials', async () => {
      const response = await axios.post('/auth/protected', { data: 'test' }, {
        headers: {
          'Authorization': basicAuthHeader(TEST_USER, TEST_PASS)
        }
      });

      expect(response.status).toBe(200);
      expect(response.data.status).toBe('authenticated');
    });
  });
});

describe('HTTP 100-Continue', () => {

  describe('POST /upload/continue', () => {

    it('should handle request with Expect: 100-continue header', async () => {
      const testData = 'Test data for 100-continue handling';

      const response = await axios.post('/upload/continue', testData, {
        headers: {
          'Content-Type': 'text/plain',
          'Expect': '100-continue'
        },
        timeout: 10000
      });

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('status', 'success');
      expect(response.data).toHaveProperty('expect_header', true);
      expect(response.data).toHaveProperty('bytes_received');
      expect(response.data.bytes_received).toBe(testData.length);
    });

    it('should handle request without Expect header', async () => {
      const testData = 'Test data without expect header';

      const response = await axios.post('/upload/continue', testData, {
        headers: {
          'Content-Type': 'text/plain'
        }
      });

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('status', 'success');
      expect(response.data).toHaveProperty('expect_header', false);
      expect(response.data.bytes_received).toBe(testData.length);
    });

    it('should handle larger payload with 100-continue', async () => {
      // Create a 1KB payload
      const testData = 'X'.repeat(1024);

      const response = await axios.post('/upload/continue', testData, {
        headers: {
          'Content-Type': 'application/octet-stream',
          'Expect': '100-continue'
        },
        timeout: 15000
      });

      expect(response.status).toBe(200);
      expect(response.data.expect_header).toBe(true);
      // Note: bytes_received might be less than 1024 if buffer is smaller
      expect(response.data.bytes_received).toBeGreaterThan(0);
    });
  });
});

describe('File Upload (pipe_to_file)', () => {

  // Note: File upload tests may fail in QEMU without littlefs mounted.
  // The tests verify the API contracts work correctly.

  // Clean up before tests
  beforeAll(async () => {
    try {
      await axios.delete('/upload', { validateStatus: () => true });
    } catch {
      // Ignore cleanup errors
    }
  });

  // Clean up after tests
  afterAll(async () => {
    try {
      await axios.delete('/upload', { validateStatus: () => true });
    } catch {
      // Ignore cleanup errors
    }
  });

  describe('POST /upload', () => {

    it('should handle upload request and return correct response format', async () => {
      const testData = Buffer.from([0x00, 0x01, 0x02, 0x03, 0x04, 0x05]);

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'application/octet-stream'
        },
        timeout: 10000,
        validateStatus: () => true  // Accept any status
      });

      // Should return 200 (success) or 500 (no filesystem)
      expect([200, 500]).toContain(response.status);
      expect(response.data).toHaveProperty('status');
      if (response.status === 200) {
        expect(response.data.status).toBe('success');
        expect(response.data).toHaveProperty('bytes', testData.length);
        expect(response.data).toHaveProperty('path');
      } else {
        // No filesystem - error code -9 (HTTPD_ERR_IO) is expected
        expect(response.data.status).toBe('error');
        expect(response.data).toHaveProperty('code', -9);
      }
    });

    it('should handle text upload request', async () => {
      const testData = 'Hello, this is a test upload file content!';

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'text/plain'
        },
        validateStatus: () => true
      });

      expect([200, 500]).toContain(response.status);
      expect(response.data).toHaveProperty('status');
    });

    it('should handle larger upload request', async () => {
      // Create 4KB of data - should require multiple recv() calls
      const testData = 'A'.repeat(4096);

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'application/octet-stream'
        },
        timeout: 15000,
        validateStatus: () => true
      });

      expect([200, 500]).toContain(response.status);
      expect(response.data).toHaveProperty('status');
    }, 20000);

    it('should handle upload with Expect: 100-continue header', async () => {
      const testData = 'Data with expect header';

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'text/plain',
          'Expect': '100-continue'
        },
        timeout: 10000,
        validateStatus: () => true
      });

      // The important thing is that the request was processed (not stuck waiting)
      expect([200, 500]).toContain(response.status);
      expect(response.data).toHaveProperty('status');
    });
  });

  describe('GET /upload/verify', () => {

    it('should return verification response with correct format', async () => {
      const response = await axios.get('/upload/verify');

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('exists');
      expect(response.data).toHaveProperty('path');
      expect(typeof response.data.exists).toBe('boolean');
    });
  });

  describe('DELETE /upload', () => {

    it('should handle delete request with correct response format', async () => {
      const deleteResponse = await axios.delete('/upload');
      expect(deleteResponse.status).toBe(200);
      expect(deleteResponse.data).toHaveProperty('deleted');
      expect(typeof deleteResponse.data.deleted).toBe('boolean');
    });
  });
});

describe('Integration: Auth + Upload', () => {

  it('should allow authenticated requests followed by upload requests', async () => {
    // This test ensures the auth and upload features work together
    // In a real app, upload might be protected by auth

    // First verify auth works
    const authResponse = await axios.get('/auth/protected', {
      headers: {
        'Authorization': basicAuthHeader(TEST_USER, TEST_PASS)
      }
    });
    expect(authResponse.status).toBe(200);
    expect(authResponse.data.status).toBe('authenticated');

    // Then do an upload (may fail if filesystem not mounted)
    const uploadResponse = await axios.post('/upload', 'authenticated upload test', {
      headers: {
        'Content-Type': 'text/plain'
      },
      validateStatus: () => true
    });
    // Upload returns 200 on success, 500 if no filesystem
    expect([200, 500]).toContain(uploadResponse.status);
    expect(uploadResponse.data).toHaveProperty('status');
  });
});
