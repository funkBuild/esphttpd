/**
 * Query Parameter Tests
 * Tests the /api/query endpoint which exercises httpd_req_get_query_string()
 * and httpd_req_get_query() APIs.
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';

describe('Query Parameters', () => {

  describe('GET /api/query with basic parameters', () => {
    it('should extract a single query parameter', async () => {
      const response = await axios.get('/api/query?name=foo');

      expect(response.status).toBe(200);
      expect(response.headers['content-type']).toContain('application/json');
      expect(response.data.raw).toBe('name=foo');
      expect(response.data.params.name).toBe('foo');
      expect(response.data.params.value).toBeNull();
      expect(response.data.params.format).toBeNull();
    });

    it('should extract multiple query parameters', async () => {
      const response = await axios.get('/api/query?name=foo&value=bar');

      expect(response.status).toBe(200);
      expect(response.data.raw).toBe('name=foo&value=bar');
      expect(response.data.params.name).toBe('foo');
      expect(response.data.params.value).toBe('bar');
      expect(response.data.params.format).toBeNull();
    });

    it('should extract all three known parameters', async () => {
      const response = await axios.get('/api/query?name=foo&value=bar&format=json');

      expect(response.status).toBe(200);
      expect(response.data.raw).toBe('name=foo&value=bar&format=json');
      expect(response.data.params.name).toBe('foo');
      expect(response.data.params.value).toBe('bar');
      expect(response.data.params.format).toBe('json');
    });
  });

  describe('GET /api/query with missing query string', () => {
    it('should return nulls when no query string is present', async () => {
      const response = await axios.get('/api/query');

      expect(response.status).toBe(200);
      expect(response.data.raw).toBeNull();
      expect(response.data.params.name).toBeNull();
      expect(response.data.params.value).toBeNull();
      expect(response.data.params.format).toBeNull();
    });
  });

  describe('GET /api/query with empty query string', () => {
    it('should handle empty query string after ?', async () => {
      const response = await axios.get('/api/query?');

      expect(response.status).toBe(200);
      // Raw may be empty string or null depending on parser behavior
      // but all named params should be null since there are no key=value pairs
      expect(response.data.params.name).toBeNull();
      expect(response.data.params.value).toBeNull();
      expect(response.data.params.format).toBeNull();
    });
  });

  describe('GET /api/query with URL-encoded characters', () => {
    it('should include encoded characters in raw query string', async () => {
      const response = await axios.get('/api/query?name=hello%20world');

      expect(response.status).toBe(200);
      // Raw query string preserves the encoding as sent over the wire
      expect(response.data.raw).toContain('name=hello');
      // The value may or may not be URL-decoded depending on the server implementation
      expect(response.data.params.name).toBeTruthy();
    });
  });

  describe('GET /api/query with empty parameter value', () => {
    it('should handle parameter with empty value', async () => {
      const response = await axios.get('/api/query?name=');

      expect(response.status).toBe(200);
      expect(response.data.raw).toBe('name=');
      // name key exists but with empty string value
      expect(response.data.params.name).toBe('');
      expect(response.data.params.value).toBeNull();
    });
  });

  describe('GET /api/query with unknown parameters', () => {
    it('should include unknown parameters in raw but not in params', async () => {
      const response = await axios.get('/api/query?unknown=test&extra=data');

      expect(response.status).toBe(200);
      // Raw query string contains everything
      expect(response.data.raw).toBe('unknown=test&extra=data');
      // Named params are not matched
      expect(response.data.params.name).toBeNull();
      expect(response.data.params.value).toBeNull();
      expect(response.data.params.format).toBeNull();
    });

    it('should show known params alongside unknown ones', async () => {
      const response = await axios.get('/api/query?name=foo&unknown=test&value=bar');

      expect(response.status).toBe(200);
      expect(response.data.raw).toBe('name=foo&unknown=test&value=bar');
      expect(response.data.params.name).toBe('foo');
      expect(response.data.params.value).toBe('bar');
      expect(response.data.params.format).toBeNull();
    });
  });
});
