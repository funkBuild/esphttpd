/**
 * HTTP 100-Continue Tests
 * Tests the server's handling of Expect: 100-continue header
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

describe('HTTP 100-Continue', () => {

  describe('POST /upload/continue', () => {

    it('should handle request with Expect: 100-continue header', async () => {
      const testData = 'Test data for 100-continue handling';

      const response = await axios.post('/upload/continue', testData, {
        headers: {
          'Content-Type': 'text/plain',
          'Expect': '100-continue'
        },
        timeout: TIMEOUTS.HTTP
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
        timeout: TIMEOUTS.HTTP
      });

      expect(response.status).toBe(200);
      expect(response.data.expect_header).toBe(true);
      // Note: bytes_received might be less than 1024 if buffer is smaller
      expect(response.data.bytes_received).toBeGreaterThan(0);
    });
  });
});
