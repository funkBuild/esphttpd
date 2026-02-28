/**
 * Performance Comparison Tests: Old Chunked API vs New Data Provider API
 *
 * Compares:
 * - /largefile/:size - Old synchronous chunked API (httpd_resp_send_chunk)
 * - /largefile-provider/:size - New data provider API with Content-Length
 * - /largefile-provider-chunked/:size - New data provider API with chunked encoding
 */

import axios from 'axios';
import { BASE_URL } from '../jest.setup';
import { verifyBlockPattern, TIMEOUTS, MIN_THROUGHPUT_KBPS } from '../test-utils';

interface PerfResult {
  endpoint: string;
  sizeKB: number;
  durationMs: number;
  throughputKBps: number;
  success: boolean;
  dataValid: boolean;
}

async function measureDownload(endpoint: string, sizeKB: number): Promise<PerfResult> {
  const startTime = Date.now();
  let success = false;
  let dataValid = false;
  let durationMs = 0;

  try {
    const response = await axios.get(`${endpoint}/${sizeKB}`, {
      responseType: 'arraybuffer',
      timeout: TIMEOUTS.PERF_CONCURRENT
    });

    durationMs = Date.now() - startTime;
    success = response.status === 200;

    if (success) {
      const data = Buffer.from(response.data);
      dataValid = data.length === sizeKB * 1024 && verifyBlockPattern(data, sizeKB);
    }
  } catch (error: any) {
    durationMs = Date.now() - startTime;
    success = false;
  }

  const throughputKBps = success ? (sizeKB / (durationMs / 1000)) : 0;

  return {
    endpoint,
    sizeKB,
    durationMs,
    throughputKBps,
    success,
    dataValid
  };
}

describe('Data Provider API Performance', () => {

  describe('Correctness Tests', () => {
    it('provider with Content-Length should return correct data', async () => {
      const response = await axios.get('/largefile-provider/50', {
        responseType: 'arraybuffer',
        timeout: TIMEOUTS.DOWNLOAD_SM
      });

      expect(response.status).toBe(200);
      expect(response.headers['content-length']).toBe((50 * 1024).toString());

      const data = Buffer.from(response.data);
      expect(data.length).toBe(50 * 1024);
      expect(verifyBlockPattern(data, 50)).toBe(true);
    }, TIMEOUTS.DOWNLOAD_SM + 5000);

    it('provider with chunked encoding should return correct data', async () => {
      const response = await axios.get('/largefile-provider-chunked/50', {
        responseType: 'arraybuffer',
        timeout: TIMEOUTS.DOWNLOAD_SM
      });

      expect(response.status).toBe(200);
      expect(response.headers['transfer-encoding']).toBe('chunked');

      const data = Buffer.from(response.data);
      expect(data.length).toBe(50 * 1024);
      expect(verifyBlockPattern(data, 50)).toBe(true);
    }, TIMEOUTS.DOWNLOAD_SM + 5000);
  });

  describe('Performance Comparison: 100KB', () => {
    const SIZE_KB = 100;
    const ITERATIONS = 3;

    let oldApiResults: PerfResult[] = [];
    let providerResults: PerfResult[] = [];
    let providerChunkedResults: PerfResult[] = [];

    beforeAll(async () => {
      // Warm up requests
      await axios.get(`/largefile/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_SM });
      await axios.get(`/largefile-provider/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_SM });
      await axios.get(`/largefile-provider-chunked/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_SM });

      // Run multiple iterations
      for (let i = 0; i < ITERATIONS; i++) {
        oldApiResults.push(await measureDownload('/largefile', SIZE_KB));
        providerResults.push(await measureDownload('/largefile-provider', SIZE_KB));
        providerChunkedResults.push(await measureDownload('/largefile-provider-chunked', SIZE_KB));
      }
    }, TIMEOUTS.PERF_SETUP);

    it('old chunked API should work correctly', () => {
      oldApiResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('new provider API (Content-Length) should work correctly', () => {
      providerResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('new provider API (chunked) should work correctly', () => {
      providerChunkedResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('should compare performance (100KB)', () => {
      const avgOld = oldApiResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;
      const avgProvider = providerResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;
      const avgProviderChunked = providerChunkedResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;

      const avgOldMs = oldApiResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;
      const avgProviderMs = providerResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;
      const avgProviderChunkedMs = providerChunkedResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;

      console.log('\n=== 100KB Performance Comparison ===');
      console.log(`Old Chunked API:           ${avgOldMs.toFixed(0)}ms, ${avgOld.toFixed(1)} KB/s`);
      console.log(`Provider (Content-Length): ${avgProviderMs.toFixed(0)}ms, ${avgProvider.toFixed(1)} KB/s`);
      console.log(`Provider (Chunked):        ${avgProviderChunkedMs.toFixed(0)}ms, ${avgProviderChunked.toFixed(1)} KB/s`);

      const improvementVsOld = ((avgProvider - avgOld) / avgOld * 100).toFixed(1);
      console.log(`\nProvider vs Old API: ${improvementVsOld}% ${parseFloat(improvementVsOld) >= 0 ? 'faster' : 'slower'}`);

      // Minimum throughput regression guard (conservative for QEMU)
      expect(avgOld).toBeGreaterThan(MIN_THROUGHPUT_KBPS);
      expect(avgProvider).toBeGreaterThan(MIN_THROUGHPUT_KBPS);
      expect(avgProviderChunked).toBeGreaterThan(MIN_THROUGHPUT_KBPS);

      // Provider API should not regress vs old API
      expect(avgProvider).toBeGreaterThanOrEqual(avgOld * 0.5);
    });
  });

  describe('Performance Comparison: 256KB', () => {
    const SIZE_KB = 256;
    const ITERATIONS = 3;

    let oldApiResults: PerfResult[] = [];
    let providerResults: PerfResult[] = [];
    let providerChunkedResults: PerfResult[] = [];

    beforeAll(async () => {
      // Warm up requests
      await axios.get(`/largefile/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_MD });
      await axios.get(`/largefile-provider/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_MD });
      await axios.get(`/largefile-provider-chunked/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_MD });

      // Run multiple iterations
      for (let i = 0; i < ITERATIONS; i++) {
        oldApiResults.push(await measureDownload('/largefile', SIZE_KB));
        providerResults.push(await measureDownload('/largefile-provider', SIZE_KB));
        providerChunkedResults.push(await measureDownload('/largefile-provider-chunked', SIZE_KB));
      }
    }, TIMEOUTS.PERF_SETUP_LARGE);

    it('old chunked API should work correctly', () => {
      oldApiResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('new provider API (Content-Length) should work correctly', () => {
      providerResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('new provider API (chunked) should work correctly', () => {
      providerChunkedResults.forEach(r => {
        expect(r.success).toBe(true);
        expect(r.dataValid).toBe(true);
      });
    });

    it('should compare performance (256KB)', () => {
      const avgOld = oldApiResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;
      const avgProvider = providerResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;
      const avgProviderChunked = providerChunkedResults.reduce((a, b) => a + b.throughputKBps, 0) / ITERATIONS;

      const avgOldMs = oldApiResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;
      const avgProviderMs = providerResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;
      const avgProviderChunkedMs = providerChunkedResults.reduce((a, b) => a + b.durationMs, 0) / ITERATIONS;

      console.log('\n=== 256KB Performance Comparison ===');
      console.log(`Old Chunked API:           ${avgOldMs.toFixed(0)}ms, ${avgOld.toFixed(1)} KB/s`);
      console.log(`Provider (Content-Length): ${avgProviderMs.toFixed(0)}ms, ${avgProvider.toFixed(1)} KB/s`);
      console.log(`Provider (Chunked):        ${avgProviderChunkedMs.toFixed(0)}ms, ${avgProviderChunked.toFixed(1)} KB/s`);

      const improvementVsOld = ((avgProvider - avgOld) / avgOld * 100).toFixed(1);
      console.log(`\nProvider vs Old API: ${improvementVsOld}% ${parseFloat(improvementVsOld) >= 0 ? 'faster' : 'slower'}`);

      // Minimum throughput regression guard (conservative for QEMU)
      expect(avgOld).toBeGreaterThan(MIN_THROUGHPUT_KBPS);
      expect(avgProvider).toBeGreaterThan(MIN_THROUGHPUT_KBPS);
      expect(avgProviderChunked).toBeGreaterThan(MIN_THROUGHPUT_KBPS);

      // Provider API should not regress vs old API
      expect(avgProvider).toBeGreaterThanOrEqual(avgOld * 0.5);
    });
  });

  describe('Concurrent Downloads Comparison', () => {
    const SIZE_KB = 64;

    it('should compare concurrent performance', async () => {
      // Old API - 3 concurrent
      const oldStart = Date.now();
      const oldPromises = [1, 2, 3].map(() =>
        axios.get(`/largefile/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_MD })
      );
      await Promise.all(oldPromises);
      const oldDuration = Date.now() - oldStart;

      // Provider API - 3 concurrent
      const providerStart = Date.now();
      const providerPromises = [1, 2, 3].map(() =>
        axios.get(`/largefile-provider/${SIZE_KB}`, { responseType: 'arraybuffer', timeout: TIMEOUTS.DOWNLOAD_MD })
      );
      await Promise.all(providerPromises);
      const providerDuration = Date.now() - providerStart;

      console.log('\n=== Concurrent Downloads (3 x 64KB) ===');
      console.log(`Old Chunked API:      ${oldDuration}ms`);
      console.log(`Provider API:         ${providerDuration}ms`);

      const improvement = ((oldDuration - providerDuration) / oldDuration * 100).toFixed(1);
      console.log(`Improvement: ${improvement}%`);

      // Both should complete within a reasonable time (10s for QEMU)
      expect(oldDuration).toBeLessThan(10000);
      expect(providerDuration).toBeLessThan(10000);

      // Provider should not be significantly slower than old API
      expect(providerDuration).toBeLessThanOrEqual(oldDuration * 2);
    }, TIMEOUTS.PERF_CONCURRENT);
  });

  describe('Large File Test (512KB)', () => {
    it('provider should handle 512KB download', async () => {
      const startTime = Date.now();
      const response = await axios.get('/largefile-provider/512', {
        responseType: 'arraybuffer',
        timeout: TIMEOUTS.DOWNLOAD_XL
      });
      const elapsed = Date.now() - startTime;

      expect(response.status).toBe(200);

      const data = Buffer.from(response.data);
      expect(data.length).toBe(512 * 1024);

      // Verify first and last blocks
      const firstBlock = data.slice(0, 11).toString();
      expect(firstBlock).toBe('BLOCK_0000:');

      const lastBlockOffset = 511 * 1024;
      const lastBlock = data.slice(lastBlockOffset, lastBlockOffset + 11).toString();
      expect(lastBlock).toBe('BLOCK_0511:');

      const throughput = (512 / (elapsed / 1000)).toFixed(1);
      console.log(`\n512KB provider download: ${elapsed}ms, ${throughput} KB/s`);
    }, TIMEOUTS.DOWNLOAD_XL + 5000);

    it('provider chunked should handle 512KB download', async () => {
      const startTime = Date.now();
      const response = await axios.get('/largefile-provider-chunked/512', {
        responseType: 'arraybuffer',
        timeout: TIMEOUTS.DOWNLOAD_XL
      });
      const elapsed = Date.now() - startTime;

      expect(response.status).toBe(200);
      expect(response.headers['transfer-encoding']).toBe('chunked');

      const data = Buffer.from(response.data);
      expect(data.length).toBe(512 * 1024);

      // Verify first and last blocks
      const firstBlock = data.slice(0, 11).toString();
      expect(firstBlock).toBe('BLOCK_0000:');

      const lastBlockOffset = 511 * 1024;
      const lastBlock = data.slice(lastBlockOffset, lastBlockOffset + 11).toString();
      expect(lastBlock).toBe('BLOCK_0511:');

      const throughput = (512 / (elapsed / 1000)).toFixed(1);
      console.log(`\n512KB provider chunked download: ${elapsed}ms, ${throughput} KB/s`);
    }, TIMEOUTS.DOWNLOAD_XL + 5000);
  });
});
