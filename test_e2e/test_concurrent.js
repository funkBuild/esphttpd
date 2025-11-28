#!/usr/bin/env node
/**
 * Concurrent Request Test for esphttpd
 *
 * Tests that the server can handle /hello requests while processing a large upload.
 * The key assertion is that hello requests complete DURING the upload.
 */

const http = require('http');

// Configuration
const DEFAULT_HOST = '127.0.0.1';
const DEFAULT_PORT = 10080;
const UPLOAD_SIZE_MB = 100;  // 100MB upload
const HELLO_REQUEST_COUNT = 50;  // More requests to verify concurrency
const MAX_HELLO_RESPONSE_TIME_MS = 5000; // 5 seconds max
const MIN_HELLO_DURING_UPLOAD = 20;  // At least 20 must complete during upload

// Parse command line args
const host = process.argv[2] || DEFAULT_HOST;
const port = parseInt(process.argv[3] || DEFAULT_PORT);

// Results tracking
const results = {
    uploadSuccess: false,
    uploadBytes: 0,
    uploadStartTime: 0,
    uploadEndTime: 0,
    uploadTimeMs: 0,
    helloResponses: [],
    helloErrors: [],
    helloDuringUpload: 0
};

function sendHelloRequest(requestId) {
    return new Promise((resolve) => {
        const startTime = Date.now();

        const req = http.request({
            host,
            port,
            path: '/hello',
            method: 'GET',
            timeout: 30000
        }, (res) => {
            let body = '';
            res.on('data', chunk => body += chunk);
            res.on('end', () => {
                const endTime = Date.now();
                resolve({
                    id: requestId,
                    success: res.statusCode === 200,
                    timeMs: endTime - startTime,
                    startTime,
                    endTime,
                    body
                });
            });
        });

        req.on('error', (err) => {
            const endTime = Date.now();
            resolve({
                id: requestId,
                success: false,
                timeMs: endTime - startTime,
                startTime,
                endTime,
                error: err.message
            });
        });

        req.on('timeout', () => {
            req.destroy();
            const endTime = Date.now();
            resolve({
                id: requestId,
                success: false,
                timeMs: endTime - startTime,
                startTime,
                endTime,
                error: 'timeout'
            });
        });

        req.end();
    });
}

function sendUpload() {
    return new Promise((resolve) => {
        const uploadSize = UPLOAD_SIZE_MB * 1024 * 1024;
        results.uploadStartTime = Date.now();

        console.log(`  Starting upload of ${UPLOAD_SIZE_MB}MB to /upload/sink...`);

        const req = http.request({
            host,
            port,
            path: '/upload/sink',
            method: 'POST',
            headers: {
                'Content-Type': 'application/octet-stream',
                'Content-Length': uploadSize
            },
            timeout: 120000
        }, (res) => {
            let body = '';
            res.on('data', chunk => body += chunk);
            res.on('end', () => {
                results.uploadEndTime = Date.now();
                results.uploadTimeMs = results.uploadEndTime - results.uploadStartTime;

                if (res.statusCode === 200) {
                    try {
                        const data = JSON.parse(body);
                        results.uploadSuccess = data.status === 'success';
                        results.uploadBytes = data.bytes || 0;
                        console.log(`  Upload complete: ${results.uploadBytes} bytes in ${results.uploadTimeMs}ms`);
                    } catch (e) {
                        console.log(`  Upload response parse error: ${body.substring(0, 100)}`);
                    }
                } else {
                    console.log(`  Upload failed with status ${res.statusCode}`);
                }
                resolve();
            });
        });

        req.on('error', (err) => {
            results.uploadEndTime = Date.now();
            results.uploadTimeMs = results.uploadEndTime - results.uploadStartTime;
            console.log(`  Upload error: ${err.message}`);
            resolve();
        });

        req.on('timeout', () => {
            req.destroy();
            results.uploadEndTime = Date.now();
            results.uploadTimeMs = results.uploadEndTime - results.uploadStartTime;
            console.log(`  Upload timeout`);
            resolve();
        });

        // Write data in chunks
        const chunkSize = 64 * 1024; // 64KB chunks
        const chunk = Buffer.alloc(chunkSize, 'X');
        let written = 0;

        function writeChunk() {
            while (written < uploadSize) {
                const toWrite = Math.min(chunkSize, uploadSize - written);
                const buf = toWrite === chunkSize ? chunk : chunk.slice(0, toWrite);
                const canContinue = req.write(buf);
                written += toWrite;

                if (!canContinue) {
                    req.once('drain', writeChunk);
                    return;
                }
            }
            req.end();
        }

        writeChunk();
    });
}

async function testConnectivity() {
    console.log(`Testing connectivity to ${host}:${port}...`);
    const result = await sendHelloRequest(0);
    if (result.success) {
        console.log(`  Connected! Response time: ${result.timeMs}ms`);
        return true;
    } else {
        console.log(`  Connection failed: ${result.error || 'unknown'}`);
        return false;
    }
}

async function runTest() {
    console.log('\n' + '='.repeat(60));
    console.log('Concurrent Request Test for esphttpd (Node.js)');
    console.log('='.repeat(60));
    console.log(`Server: ${host}:${port}`);
    console.log(`Upload size: ${UPLOAD_SIZE_MB}MB`);
    console.log(`Hello requests: ${HELLO_REQUEST_COUNT}`);
    console.log(`Max acceptable hello time: ${MAX_HELLO_RESPONSE_TIME_MS}ms`);
    console.log(`Min hello during upload: ${MIN_HELLO_DURING_UPLOAD} (KEY ASSERTION)`);
    console.log('='.repeat(60) + '\n');

    // Start upload (don't await yet)
    const uploadPromise = sendUpload();

    // Wait a moment for upload to start
    await new Promise(r => setTimeout(r, 500));

    // Send hello requests while upload is running
    console.log(`  Sending ${HELLO_REQUEST_COUNT} /hello requests during upload...`);

    const helloPromises = [];
    for (let i = 0; i < HELLO_REQUEST_COUNT; i++) {
        helloPromises.push(sendHelloRequest(i));
        // Small delay between requests
        await new Promise(r => setTimeout(r, 100));
    }

    // Wait for all hello requests
    const helloResults = await Promise.all(helloPromises);

    // Process hello results
    for (const r of helloResults) {
        if (r.success) {
            results.helloResponses.push(r);
        } else {
            results.helloErrors.push(r);
        }
    }

    // Wait for upload to complete
    await uploadPromise;

    // Calculate statistics
    if (results.helloResponses.length > 0) {
        const times = results.helloResponses.map(r => r.timeMs);
        results.maxHelloTimeMs = Math.max(...times);
        results.avgHelloTimeMs = times.reduce((a, b) => a + b, 0) / times.length;

        // Count hello requests that completed DURING the upload
        for (const r of results.helloResponses) {
            if (r.endTime >= results.uploadStartTime && r.endTime <= results.uploadEndTime) {
                results.helloDuringUpload++;
            }
        }
    }

    // Print results
    console.log('\n' + '='.repeat(60));
    console.log('TEST RESULTS');
    console.log('='.repeat(60));

    console.log('\nUpload Results:');
    console.log(`  Success: ${results.uploadSuccess}`);
    console.log(`  Bytes uploaded: ${results.uploadBytes.toLocaleString()}`);
    console.log(`  Time: ${results.uploadTimeMs}ms`);
    console.log(`  Time window: ${results.uploadStartTime} - ${results.uploadEndTime}`);
    if (results.uploadTimeMs > 0) {
        const rateMbps = (results.uploadBytes * 8) / (results.uploadTimeMs / 1000) / 1_000_000;
        console.log(`  Rate: ${rateMbps.toFixed(2)} Mbps`);
    }

    console.log('\nHello Request Results:');
    console.log(`  Successful: ${results.helloResponses.length}/${HELLO_REQUEST_COUNT}`);
    console.log(`  Failed: ${results.helloErrors.length}`);
    if (results.helloResponses.length > 0) {
        console.log(`  Average response time: ${results.avgHelloTimeMs.toFixed(1)}ms`);
        console.log(`  Max response time: ${results.maxHelloTimeMs.toFixed(1)}ms`);
        console.log(`  *** Completed DURING upload: ${results.helloDuringUpload}/${results.helloResponses.length} ***`);

        console.log('\n  Timing details (first 5):');
        const sorted = [...results.helloResponses].sort((a, b) => a.endTime - b.endTime);
        for (const r of sorted.slice(0, 5)) {
            const during = r.endTime >= results.uploadStartTime && r.endTime <= results.uploadEndTime
                ? 'DURING' : 'AFTER';
            console.log(`    Request ${r.id}: completed at ${r.endTime} (${r.timeMs}ms) - ${during} upload`);
        }
    }

    if (results.helloErrors.length > 0) {
        console.log('\n  Errors:');
        for (const err of results.helloErrors.slice(0, 5)) {
            console.log(`    Request ${err.id}: ${err.error || 'unknown'}`);
        }
    }

    // Detailed timestamp log for verification
    console.log('\n  === TIMESTAMP VERIFICATION ===');
    console.log(`  Upload started:   ${results.uploadStartTime}`);
    console.log(`  Upload completed: ${results.uploadEndTime}`);
    console.log(`  Upload duration:  ${results.uploadTimeMs}ms`);
    console.log('\n  Hello request completion times (sorted):');
    const allSorted = [...results.helloResponses].sort((a, b) => a.endTime - b.endTime);
    for (const r of allSorted) {
        const relativeMs = r.endTime - results.uploadStartTime;
        const during = r.endTime >= results.uploadStartTime && r.endTime <= results.uploadEndTime;
        const marker = during ? 'DURING' : 'AFTER ';
        console.log(`    Hello #${String(r.id).padStart(2)}: +${String(relativeMs).padStart(6)}ms (${r.timeMs}ms response) [${marker}]`);
    }
    console.log(`\n  Upload end relative: +${results.uploadTimeMs}ms`);

    // Determine pass/fail
    console.log('\n' + '='.repeat(60));

    let passed = true;
    const reasons = [];

    if (!results.uploadSuccess) {
        passed = false;
        reasons.push('Upload failed');
    }

    if (results.helloResponses.length < HELLO_REQUEST_COUNT * 0.6) {
        passed = false;
        reasons.push(`Too many hello failures (${results.helloErrors.length}/${HELLO_REQUEST_COUNT})`);
    }

    if (results.maxHelloTimeMs > MAX_HELLO_RESPONSE_TIME_MS) {
        passed = false;
        reasons.push(`Hello response too slow (${results.maxHelloTimeMs.toFixed(0)}ms > ${MAX_HELLO_RESPONSE_TIME_MS}ms)`);
    }

    if (results.helloDuringUpload < MIN_HELLO_DURING_UPLOAD) {
        passed = false;
        reasons.push(
            `Not enough hello requests completed DURING upload ` +
            `(${results.helloDuringUpload} < ${MIN_HELLO_DURING_UPLOAD} required). ` +
            `Server may be blocking on upload.`
        );
    }

    if (passed) {
        console.log('TEST PASSED: Server handled concurrent requests during upload!');
        console.log(`  - ${results.helloDuringUpload} /hello requests completed DURING the upload`);
        console.log(`  - Max response time ${results.maxHelloTimeMs.toFixed(0)}ms < ${MAX_HELLO_RESPONSE_TIME_MS}ms threshold`);
        console.log(`  - Upload completed successfully (${results.uploadBytes.toLocaleString()} bytes)`);
    } else {
        console.log('TEST FAILED:');
        for (const reason of reasons) {
            console.log(`  - ${reason}`);
        }
    }

    console.log('='.repeat(60) + '\n');

    return passed ? 0 : 1;
}

async function main() {
    if (!await testConnectivity()) {
        console.log('\nCannot connect to server. Make sure the E2E server is running.');
        process.exit(1);
    }

    const exitCode = await runTest();
    process.exit(exitCode);
}

main().catch(err => {
    console.error('Fatal error:', err);
    process.exit(1);
});
