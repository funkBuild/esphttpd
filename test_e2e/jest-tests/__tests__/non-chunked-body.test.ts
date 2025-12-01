/**
 * Tests for httpd_resp_send with large response bodies (non-chunked)
 *
 * These tests verify that httpd_resp_send correctly delivers complete responses
 * when the body is larger than the socket buffer (~1440 bytes). This is critical
 * because partial responses would be silently truncated.
 *
 * Background: When using non-blocking sends, if the socket buffer fills up,
 * remaining data is queued to be sent later. However, if the handler returns
 * before the queue is drained, the client receives a truncated response.
 *
 * The /large-body/:size endpoint uses httpd_resp_send (not chunked encoding)
 * to send a response of the specified size in bytes.
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';

describe('Non-Chunked Large Body Responses', () => {

  // Helper to verify the pattern used by /large-body/:size
  function verifyPattern(data: Buffer, expectedSize: number): { valid: boolean; error?: string } {
    if (data.length !== expectedSize) {
      return { valid: false, error: `Length mismatch: expected ${expectedSize}, got ${data.length}` };
    }

    // Verify position markers every 256 bytes (format: "@XX:" - 4 bytes)
    // @ followed by 2-digit position and colon
    const numMarkers = Math.min(Math.floor(expectedSize / 256), 64);
    for (let i = 0; i < numMarkers; i++) {
      const offset = i * 256;
      const expected = `@${Math.floor(i / 10)}${i % 10}:`;
      const actual = data.slice(offset, offset + 4).toString();
      if (actual !== expected) {
        return {
          valid: false,
          error: `Marker mismatch at offset ${offset}: expected '${expected}', got '${actual}'`
        };
      }
    }

    return { valid: true };
  }

  describe('Responses larger than socket buffer', () => {
    // Socket buffer is typically ~1440 bytes (TCP MSS)
    // These tests verify complete delivery of responses larger than that

    it('should deliver complete 2KB response', async () => {
      const size = 2048;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);

      const data = Buffer.from(response.data);
      expect(data.length).toBe(size);

      const result = verifyPattern(data, size);
      expect(result.valid).toBe(true);
      if (!result.valid) {
        console.error(result.error);
      }
    });

    it('should deliver complete 4KB response', async () => {
      const size = 4096;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);

      const data = Buffer.from(response.data);
      expect(data.length).toBe(size);

      const result = verifyPattern(data, size);
      expect(result.valid).toBe(true);
    });

    it('should deliver complete 8KB response', async () => {
      const size = 8192;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 15000
      });

      expect(response.status).toBe(200);

      const data = Buffer.from(response.data);
      expect(data.length).toBe(size);

      const result = verifyPattern(data, size);
      expect(result.valid).toBe(true);
    });

    it('should deliver complete 16KB response', async () => {
      const size = 16384;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 20000
      });

      expect(response.status).toBe(200);

      const data = Buffer.from(response.data);
      expect(data.length).toBe(size);

      const result = verifyPattern(data, size);
      expect(result.valid).toBe(true);
    });
  });

  describe('Content-Length integrity', () => {
    it('should match Content-Length header with actual body size', async () => {
      const size = 4096;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      const contentLength = parseInt(response.headers['content-length']);
      const actualLength = Buffer.from(response.data).length;

      expect(contentLength).toBe(size);
      expect(actualLength).toBe(contentLength);
    });

    it('should have matching X-Expected-Length header', async () => {
      const size = 4096;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      // The server sets X-Expected-Length to the requested size
      const expectedLength = response.headers['x-expected-length'];
      expect(expectedLength).toBe(size.toString());
    });
  });

  describe('Edge cases around socket buffer size', () => {
    // Test sizes around the typical socket buffer boundary (~1440 bytes)

    it('should deliver 1KB response (below socket buffer)', async () => {
      const size = 1024;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);
      expect(Buffer.from(response.data).length).toBe(size);
    });

    it('should deliver 1500 byte response (just above typical socket buffer)', async () => {
      const size = 1500;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);
      expect(Buffer.from(response.data).length).toBe(size);
    });

    it('should deliver 1440 byte response (typical socket buffer size)', async () => {
      const size = 1440;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);
      expect(Buffer.from(response.data).length).toBe(size);
    });

    it('should deliver 2880 byte response (2x socket buffer)', async () => {
      const size = 2880;
      const response = await axios.get(`/large-body/${size}`, {
        responseType: 'arraybuffer',
        timeout: 10000
      });

      expect(response.status).toBe(200);
      expect(Buffer.from(response.data).length).toBe(size);
    });
  });

  describe('Concurrent non-chunked requests', () => {
    it('should handle multiple concurrent large body responses', async () => {
      const sizes = [2048, 3000, 4096, 5000];

      const requests = sizes.map(size =>
        axios.get(`/large-body/${size}`, {
          responseType: 'arraybuffer',
          timeout: 15000
        })
      );

      const responses = await Promise.all(requests);

      for (let i = 0; i < responses.length; i++) {
        const response = responses[i];
        const expectedSize = sizes[i];

        expect(response.status).toBe(200);

        const data = Buffer.from(response.data);
        expect(data.length).toBe(expectedSize);

        const result = verifyPattern(data, expectedSize);
        expect(result.valid).toBe(true);
      }
    });
  });

  describe('Rapid sequential requests', () => {
    it('should handle rapid sequential large body requests', async () => {
      const sizes = [2048, 4096, 2048, 4096, 2048];

      for (const size of sizes) {
        const response = await axios.get(`/large-body/${size}`, {
          responseType: 'arraybuffer',
          timeout: 10000
        });

        expect(response.status).toBe(200);
        expect(Buffer.from(response.data).length).toBe(size);
      }
    });
  });
});
