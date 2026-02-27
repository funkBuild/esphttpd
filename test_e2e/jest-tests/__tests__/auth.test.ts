/**
 * Basic Authentication Tests
 * Tests for HTTP Basic Auth challenge/response
 */

import axios, { AxiosError } from 'axios';
import { BASE_URL } from '../jest.setup';
import { basicAuthHeader, TEST_USER, TEST_PASS, TIMEOUTS } from '../test-utils';

describe('Basic Authentication', () => {

  describe('GET /auth/protected', () => {

    it('should return 401 without credentials', async () => {
      const response = await axios.get('/auth/protected', {
        validateStatus: () => true
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
