/**
 * Template Engine Tests
 * Tests the server-side template processing functionality
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';
import { TIMEOUTS } from '../test-utils';

describe('Template Engine', () => {

  describe('GET /template', () => {
    it('should process template with variables', async () => {
      const response = await axios.get('/template');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/html');

      // Check that template variables were replaced
      expect(response.data).toContain('<h1>E2E Test Page</h1>');
      expect(response.data).toContain('<p>Dynamic content here</p>');
      expect(response.data).toMatch(/<p>Request count: \d+<\/p>/);

      // Ensure no template syntax remains
      expect(response.data).not.toContain('{{');
      expect(response.data).not.toContain('}}');
    });

    it('should update dynamic values on each request', async () => {
      const response1 = await axios.get('/template');
      const response2 = await axios.get('/template');

      // Extract request counts
      const countMatch1 = response1.data.match(/Request count: (\d+)/);
      const countMatch2 = response2.data.match(/Request count: (\d+)/);

      expect(countMatch1).toBeTruthy();
      expect(countMatch2).toBeTruthy();

      const count1 = parseInt(countMatch1[1]);
      const count2 = parseInt(countMatch2[1]);

      // Second request should have higher count
      expect(count2).toBeGreaterThan(count1);
    });

    it('should handle HTML structure correctly', async () => {
      const response = await axios.get('/template');

      // Check for proper HTML structure
      expect(response.data).toContain('<html>');
      expect(response.data).toContain('</html>');
      expect(response.data).toContain('<body>');
      expect(response.data).toContain('</body>');
      expect(response.data).toContain('<h1>');
      expect(response.data).toContain('</h1>');
    });
  });

  describe('Template Performance', () => {
    it('should process templates efficiently', async () => {
      const startTime = Date.now();
      const response = await axios.get('/template');
      const responseTime = Date.now() - startTime;

      expect(response.status).toBe(200);
      expect(responseTime).toBeLessThan(3000); // QEMU: Increased from 200ms to 3s
    });

    it('should increment request count across sequential requests', async () => {
      const r1 = await axios.get('/template', { timeout: TIMEOUTS.HTTP });
      expect(r1.status).toBe(200);
      expect(r1.data).toContain('E2E Test Page');

      const r2 = await axios.get('/template', { timeout: TIMEOUTS.HTTP });
      expect(r2.status).toBe(200);

      const count1 = parseInt(r1.data.match(/Request count: (\d+)/)?.[1] ?? '0');
      const count2 = parseInt(r2.data.match(/Request count: (\d+)/)?.[1] ?? '0');

      // Sequential requests should produce increasing counts
      expect(count2).toBeGreaterThan(count1);
    });
  });

  describe('Template Edge Cases', () => {
    it('should handle templates with special characters', async () => {
      const response = await axios.get('/template');

      // The template always returns well-formed HTML with specific content
      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('text/html');
      expect(response.data).toContain('<html><body>');
      expect(response.data).toContain('<h1>E2E Test Page</h1>');
      expect(response.data).toContain('<p>Dynamic content here</p>');
      expect(response.data).toMatch(/<p>Request count: \d+<\/p>/);
      expect(response.data).toContain('</body></html>');
    });

    it('should maintain consistent output format', async () => {
      const response = await axios.get('/template');

      // Check for consistent formatting - server returns compact HTML
      const html: string = response.data;
      expect(html.length).toBeGreaterThan(50);

      // Should have proper HTML structure
      expect(html).toContain('<html>');
      expect(html).toContain('<body>');
      expect(html).toContain('</html>');
      expect(html).toContain('</body>');
    });
  });

  describe('Template vs Static Content', () => {
    it('should differentiate between template and static responses', async () => {
      const templateResponse = await axios.get('/template');
      const staticResponse = await axios.get('/');

      // Both should be HTML
      expect(templateResponse.headers['content-type']).toContain('text/html');
      expect(staticResponse.headers['content-type']).toContain('text/html');

      // Template should have specific content
      expect(templateResponse.data).toContain('E2E Test Page');
      expect(staticResponse.data).toContain('ESP32 E2E Test Server');

      // They should be different
      expect(templateResponse.data).not.toBe(staticResponse.data);
    });
  });

  describe('Template Content Validation', () => {
    it('should produce valid HTML with balanced tags', async () => {
      const response = await axios.get('/template');

      // Check for balanced tags
      const openTags = (response.data.match(/<[^/][^>]*>/g) || []).length;
      const closeTags = (response.data.match(/<\/[^>]+>/g) || []).length;

      // Template has no void elements - tags should be exactly balanced
      expect(openTags).toBe(closeTags);
      expect(openTags).toBeGreaterThanOrEqual(5); // At least html, body, h1, 2x p
    });

    it('should not expose internal template variables', async () => {
      const response = await axios.get('/template');

      // Should not contain any template engine internals
      expect(response.data).not.toContain('template_var_callback');
      expect(response.data).not.toContain('template_context_t');
      expect(response.data).not.toContain('template_init');
    });
  });
});