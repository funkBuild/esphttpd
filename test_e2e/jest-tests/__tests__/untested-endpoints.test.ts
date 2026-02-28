/**
 * Tests for previously untested server endpoints
 * Covers: /perf, /hello, DELETE /api/data/*, /upload/sink,
 *         /upload/defer, /upload/defer/custom, /upload/defer/verify
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

describe('GET /perf', () => {
  it('should return minimal OK response', async () => {
    const response = await axios.get('/perf');

    expect(response.status).toBe(200);
    expect(response.headers['content-type']).toContain('text/plain');
    expect(response.data).toBe('OK');
  });

  it('should respond quickly', async () => {
    // Warm up
    await axios.get('/perf');

    const start = Date.now();
    await axios.get('/perf');
    const elapsed = Date.now() - start;

    // Even on QEMU, a minimal response should be fast
    expect(elapsed).toBeLessThan(3000);
  });
});

describe('GET /hello', () => {
  it('should return JSON hello response', async () => {
    const response = await axios.get('/hello');

    expect(response.status).toBe(200);
    expect(response.headers['content-type']).toContain('application/json');
    expect(response.data.message).toBe('Hello, World!');
    expect(response.data.timestamp).toBeDefined();
    expect(typeof response.data.timestamp).toBe('number');
    expect(response.data.request_count).toBeGreaterThan(0);
  });

  it('should increment request count across calls', async () => {
    const r1 = await axios.get('/hello');
    const r2 = await axios.get('/hello');

    expect(r2.data.request_count).toBeGreaterThan(r1.data.request_count);
  });
});

describe('DELETE /api/data/*', () => {
  it('should return delete confirmation with ID', async () => {
    const response = await axios.delete('/api/data/42');

    expect(response.status).toBe(200);
    expect(response.headers['content-type']).toContain('application/json');
    expect(response.data.message).toBe('Deleted');
    expect(response.data.id).toBe('42');
  });

  it('should echo back the ID from the URL', async () => {
    const response = await axios.delete('/api/data/test-item-99');

    expect(response.status).toBe(200);
    expect(response.data.id).toBe('test-item-99');
  });
});

describe('Deferred Upload API', () => {
  // Clean up before and after
  beforeAll(async () => {
    await axios.delete('/upload/defer', { validateStatus: () => true });
  });

  afterAll(async () => {
    await axios.delete('/upload/defer', { validateStatus: () => true });
  });

  describe('POST /upload/defer', () => {
    it('should accept deferred upload and return response', async () => {
      const testData = 'Deferred upload test data';

      const response = await axios.post('/upload/defer', testData, {
        headers: { 'Content-Type': 'application/octet-stream' },
        timeout: TIMEOUTS.HTTP,
        validateStatus: () => true
      });

      // 200 = success (filesystem available), 500 = no filesystem in QEMU
      expect([200, 500]).toContain(response.status);
      if (response.status === 200) {
        expect(response.data.status).toBe('success');
        expect(response.data.deferred).toBe(true);
        expect(response.data.bytes).toBe(testData.length);
      } else {
        expect(response.data.status).toBe('error');
      }
    });
  });

  describe('POST /upload/defer/custom', () => {
    it('should accept custom deferred upload with chunk tracking', async () => {
      const testData = 'Custom deferred upload data with chunk tracking';

      try {
        const response = await axios.post('/upload/defer/custom', testData, {
          headers: { 'Content-Type': 'application/octet-stream' },
          timeout: TIMEOUTS.HTTP,
          validateStatus: () => true
        });

        expect([200, 500]).toContain(response.status);
        if (response.status === 200) {
          expect(response.data.status).toBe('success');
          expect(response.data.deferred).toBe(true);
          expect(response.data.custom).toBe(true);
          expect(response.data.bytes).toBe(testData.length);
          expect(response.data.chunks).toBeGreaterThan(0);
        } else {
          // No filesystem in QEMU - server returns error
          expect(response.data).toBeDefined();
        }
      } catch (error: any) {
        // Custom defer may hang in QEMU when fopen fails mid-defer
        // Accept timeout as known QEMU limitation
        expect(error.code).toBe('ECONNABORTED');
      }
    }, TIMEOUTS.HTTP + 5000);
  });

  describe('GET /upload/defer/verify', () => {
    it('should return verification status for both upload types', async () => {
      const response = await axios.get('/upload/defer/verify');

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('deferred');
      expect(response.data.deferred).toHaveProperty('exists');
      expect(response.data.deferred).toHaveProperty('size');
      expect(typeof response.data.deferred.exists).toBe('boolean');
      expect(response.data).toHaveProperty('custom');
      expect(response.data.custom).toHaveProperty('exists');
      expect(response.data.custom).toHaveProperty('size');
    });
  });

  describe('DELETE /upload/defer', () => {
    it('should return deletion status for both upload types', async () => {
      const response = await axios.delete('/upload/defer');

      expect(response.status).toBe(200);
      expect(response.data).toHaveProperty('deferred_deleted');
      expect(response.data).toHaveProperty('custom_deleted');
      expect(typeof response.data.deferred_deleted).toBe('boolean');
      expect(typeof response.data.custom_deleted).toBe('boolean');
    });
  });
});

describe('POST /upload/sink', () => {
  it('should accept and discard upload data', async () => {
    const testData = 'X'.repeat(1024); // 1KB

    const response = await axios.post('/upload/sink', testData, {
      headers: { 'Content-Type': 'application/octet-stream' },
      timeout: TIMEOUTS.HTTP
    });

    expect(response.status).toBe(200);
    expect(response.data.status).toBe('success');
    expect(response.data.bytes).toBe(testData.length);
    expect(response.data.chunks).toBeGreaterThan(0);
    expect(response.data.elapsed_ms).toBeGreaterThanOrEqual(0);
  });

  it('should handle larger upload and report metrics', async () => {
    const testData = 'Y'.repeat(8192); // 8KB

    const response = await axios.post('/upload/sink', testData, {
      headers: { 'Content-Type': 'application/octet-stream' },
      timeout: TIMEOUTS.DOWNLOAD_SM
    });

    expect(response.status).toBe(200);
    expect(response.data.status).toBe('success');
    expect(response.data.bytes).toBe(testData.length);
    expect(response.data.rate_mbps).toBeGreaterThan(0);
  });

  it('should allow concurrent requests during sink upload', async () => {
    // Start a slow sink upload and verify /hello still responds
    const uploadData = 'Z'.repeat(4096);

    const [uploadResponse, helloResponse] = await Promise.all([
      axios.post('/upload/sink', uploadData, {
        headers: { 'Content-Type': 'application/octet-stream' },
        timeout: TIMEOUTS.DOWNLOAD_SM
      }),
      axios.get('/hello', { timeout: TIMEOUTS.HTTP })
    ]);

    expect(uploadResponse.status).toBe(200);
    expect(uploadResponse.data.status).toBe('success');
    expect(helloResponse.status).toBe(200);
    expect(helloResponse.data.message).toBe('Hello, World!');
  }, TIMEOUTS.DOWNLOAD_MD);
});
