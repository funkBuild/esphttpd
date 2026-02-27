/**
 * File Upload Tests (pipe_to_file)
 * Tests for file upload, verification, and deletion
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';
import { basicAuthHeader, TEST_USER, TEST_PASS, TIMEOUTS } from '../test-utils';

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
        timeout: TIMEOUTS.HTTP,
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
      if (response.status === 200) {
        expect(response.data.status).toBe('success');
        expect(response.data).toHaveProperty('bytes', testData.length);
      } else {
        // No filesystem in QEMU - verify proper error format
        expect(response.data.status).toBe('error');
        expect(response.data).toHaveProperty('code', -9);
      }
    });

    it('should handle larger upload request', async () => {
      // Create 4KB of data - should require multiple recv() calls
      const testData = 'A'.repeat(4096);

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'application/octet-stream'
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      expect([200, 500]).toContain(response.status);
      if (response.status === 200) {
        expect(response.data.status).toBe('success');
        expect(response.data).toHaveProperty('bytes', testData.length);
      } else {
        // No filesystem in QEMU - verify proper error format
        expect(response.data.status).toBe('error');
        expect(response.data).toHaveProperty('code', -9);
      }
    }, TIMEOUTS.HTTP + 5000);

    it('should handle upload with Expect: 100-continue header', async () => {
      const testData = 'Data with expect header';

      const response = await axios.post('/upload', testData, {
        headers: {
          'Content-Type': 'text/plain',
          'Expect': '100-continue'
        },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      // The important thing is that the request was processed (not stuck waiting)
      expect([200, 500]).toContain(response.status);
      if (response.status === 200) {
        expect(response.data.status).toBe('success');
        expect(response.data).toHaveProperty('bytes', testData.length);
      } else {
        // No filesystem in QEMU - verify proper error format
        expect(response.data.status).toBe('error');
        expect(response.data).toHaveProperty('code', -9);
      }
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
    if (uploadResponse.status === 200) {
      expect(uploadResponse.data.status).toBe('success');
    } else {
      expect(uploadResponse.data.status).toBe('error');
      expect(uploadResponse.data).toHaveProperty('code', -9);
    }
  });
});
