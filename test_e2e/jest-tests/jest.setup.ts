// Jest setup file
// This runs before all tests

import axios from 'axios';

// Base URL for the ESP32 server (QEMU)
export const BASE_URL = process.env.SERVER_URL || 'http://127.0.0.1:9000';

// Configure axios defaults
axios.defaults.baseURL = BASE_URL;
axios.defaults.timeout = 15000; // Increased for QEMU emulation

// Helper to check if server is running
export async function waitForServer(maxRetries = 10, delay = 1000): Promise<void> {
  for (let i = 0; i < maxRetries; i++) {
    try {
      await axios.get('/');
      console.log('✓ Server is responding');
      return;
    } catch (error) {
      if (i === maxRetries - 1) {
        throw new Error(`Server not responding at ${BASE_URL}. Make sure QEMU is running with: cd ../&& ./run_e2e_server.sh`);
      }
      console.log(`Waiting for server... (attempt ${i + 1}/${maxRetries})`);
      await new Promise(resolve => setTimeout(resolve, delay));
    }
  }
}

// Check server before running tests
beforeAll(async () => {
  console.log(`Testing against server at: ${BASE_URL}`);
  await waitForServer();
});
