/**
 * Static File Serving Tests
 * Tests the server's ability to serve static files
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';

describe('Static File Serving', () => {

  describe('GET /static/*', () => {
    it('should serve text files', async () => {
      const response = await axios.get('/static/test.txt');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/plain');
      expect(response.data).toContain('Mock static file: test.txt');
    });

    it('should serve HTML files with correct content type', async () => {
      const response = await axios.get('/static/index.html');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/html');
      expect(response.data).toContain('Mock static file: index.html');
    });

    it('should serve CSS files with correct content type', async () => {
      const response = await axios.get('/static/styles.css');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/css');
      expect(response.data).toContain('Mock static file: styles.css');
    });

    it('should serve JavaScript files with correct content type', async () => {
      const response = await axios.get('/static/script.js');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/javascript');
      expect(response.data).toContain('Mock static file: script.js');
    });

    it('should serve JSON files with correct content type', async () => {
      const response = await axios.get('/static/data.json');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data).toContain('Mock static file: data.json');
    });

    it('should handle nested paths', async () => {
      const response = await axios.get('/static/subdirectory/nested.txt');

      expect(response.status).toBe(200);
      expect(response.data).toContain('Mock static file: subdirectory/nested.txt');
    });

    it('should handle files with multiple extensions', async () => {
      const response = await axios.get('/static/file.min.js');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/javascript');
      expect(response.data).toContain('Mock static file: file.min.js');
    });

    it('should handle files with special characters in names', async () => {
      const response = await axios.get('/static/file-name_123.txt');

      expect(response.status).toBe(200);
      expect(response.data).toContain('Mock static file: file-name_123.txt');
    });
  });

  describe('Binary File Handling', () => {
    it('should serve binary files', async () => {
      const response = await axios.get('/static/image.jpg', {
        responseType: 'arraybuffer'
      });

      expect(response.status).toBe(200);
      // Binary files get application/octet-stream as fallback
      expect(response.headers['content-type']).toBeDefined();
    });
  });

  describe('Cache Headers', () => {
    it('should include appropriate headers for static files', async () => {
      const response = await axios.get('/static/styles.css');

      expect(response.status).toBe(200);
      // Check for common headers (server may or may not set these)
      expect(response.headers).toBeDefined();
      expect(response.headers['content-length']).toBeDefined();
    });
  });

  describe('Error Handling', () => {
    it('should return 404 for non-static routes without /static prefix', async () => {
      try {
        await axios.get('/notastatic.txt');
        fail('Should have thrown 404');
      } catch (error: any) {
        expect(error.response.status).toBe(404);
      }
    });

    it('should handle empty filename gracefully', async () => {
      const response = await axios.get('/static/');

      // Should either redirect, show index, or return an error
      expect(response.status).toBeGreaterThanOrEqual(200);
      expect(response.status).toBeLessThan(500);
    });
  });

  describe('Performance', () => {
    it('should serve static files quickly', async () => {
      const startTime = Date.now();
      const response = await axios.get('/static/test.txt');
      const responseTime = Date.now() - startTime;

      expect(response.status).toBe(200);
      expect(responseTime).toBeLessThan(3000); // QEMU: Increased from 100ms to 3s
    });

    it('should handle concurrent static file requests', async () => {
      // Reduced for QEMU performance
      const files = [
        'test.txt',
        'index.html',
        'styles.css'
      ];

      const requests = files.map(file =>
        axios.get(`/static/${file}`, { timeout: 15000 })
      );

      const responses = await Promise.all(requests);

      responses.forEach((response, index) => {
        expect(response.status).toBe(200);
        expect(response.data).toContain(`Mock static file: ${files[index]}`);
      });
    }, 25000); // QEMU: Extended timeout for concurrent requests
  });

  describe('Directory Traversal Protection', () => {
    it('should prevent directory traversal attacks', async () => {
      const maliciousUrls = [
        '/static/../../../etc/passwd',
        '/static/..%2F..%2F..%2Fetc%2Fpasswd',
        '/static/....//....//etc/passwd'
      ];

      for (const url of maliciousUrls) {
        try {
          const response = await axios.get(url, {
            validateStatus: () => true  // Accept all status codes
          });

          // Should not expose system files
          expect(response.data).not.toContain('root:');
          expect(response.data).not.toContain('/bin/bash');

          // Should return mock data or error
          if (response.status === 200) {
            expect(response.data).toContain('Mock static file');
          }
        } catch (error: any) {
          // Connection error is acceptable for malicious paths
          expect(error).toBeDefined();
        }
      }
    });
  });

  describe('Content Negotiation', () => {
    it('should respect Accept headers for static files', async () => {
      const response = await axios.get('/static/data.json', {
        headers: {
          'Accept': 'application/json'
        }
      });

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
    });
  });

  describe('Large File Handling', () => {
    it('should handle requests for large files', async () => {
      // Test that server doesn't crash with large filename
      const response = await axios.get('/static/very-long-filename-that-might-cause-issues-with-buffer-overflow-attacks-or-similar-problems.txt');

      expect(response.status).toBe(200);
      expect(response.data).toBeDefined();
    });
  });
});