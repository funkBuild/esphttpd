#!/usr/bin/env python3
"""
Performance benchmark for esphttpd E2E server
Measures requests per second over a 5 second period
"""

import urllib.request
import time
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
import statistics

import os
HOST = os.environ.get("HOST", "http://localhost:8080")
ENDPOINT = "/perf"
DURATION = 5  # seconds

class BenchmarkResults:
    def __init__(self):
        self.success = 0
        self.errors = 0
        self.latencies = []
        self.lock = threading.Lock()

    def record_success(self, latency):
        with self.lock:
            self.success += 1
            self.latencies.append(latency)

    def record_error(self):
        with self.lock:
            self.errors += 1

def make_request(url, results):
    """Make a single HTTP request and record the result"""
    try:
        start = time.perf_counter()
        req = urllib.request.urlopen(url, timeout=5)
        _ = req.read()
        latency = (time.perf_counter() - start) * 1000  # ms
        results.record_success(latency)
        return True
    except Exception as e:
        results.record_error()
        return False

def run_benchmark(concurrency):
    """Run benchmark with specified concurrency level"""
    url = f"{HOST}{ENDPOINT}"
    results = BenchmarkResults()

    # Verify server is running
    try:
        urllib.request.urlopen(url, timeout=2)
    except Exception as e:
        print(f"Error: Server not responding at {HOST}")
        print(f"Make sure the E2E server is running")
        return None

    print(f"\n  Concurrency: {concurrency}")
    print(f"  Duration:    {DURATION}s")
    print("  Running...", end="", flush=True)

    start_time = time.time()
    end_time = start_time + DURATION

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = []
        while time.time() < end_time:
            # Submit new requests to keep workers busy
            for _ in range(concurrency - len([f for f in futures if not f.done()])):
                if time.time() >= end_time:
                    break
                futures.append(executor.submit(make_request, url, results))

            # Brief sleep to prevent CPU spin
            time.sleep(0.001)

    # Wait for all pending requests
    for f in futures:
        f.result()

    actual_duration = time.time() - start_time
    print(" Done!")

    # Calculate statistics
    rps = results.success / actual_duration
    total = results.success + results.errors

    print(f"\n  Results:")
    print(f"    Complete requests:  {results.success}")
    print(f"    Failed requests:    {results.errors}")
    print(f"    Requests/second:    {rps:.2f}")

    if results.latencies:
        avg_latency = statistics.mean(results.latencies)
        min_latency = min(results.latencies)
        max_latency = max(results.latencies)
        p50 = statistics.median(results.latencies)
        sorted_lat = sorted(results.latencies)
        p95_idx = int(len(sorted_lat) * 0.95)
        p99_idx = int(len(sorted_lat) * 0.99)
        p95 = sorted_lat[p95_idx] if p95_idx < len(sorted_lat) else max_latency
        p99 = sorted_lat[p99_idx] if p99_idx < len(sorted_lat) else max_latency

        print(f"    Avg latency:        {avg_latency:.2f}ms")
        print(f"    Min latency:        {min_latency:.2f}ms")
        print(f"    Max latency:        {max_latency:.2f}ms")
        print(f"    P50 latency:        {p50:.2f}ms")
        print(f"    P95 latency:        {p95:.2f}ms")
        print(f"    P99 latency:        {p99:.2f}ms")

    return rps

def main():
    print("=" * 60)
    print("         HTTP Performance Benchmark")
    print("=" * 60)
    print(f"Host: {HOST}")
    print(f"Endpoint: {ENDPOINT}")

    results = {}

    # Test with different concurrency levels
    for concurrency in [1, 4, 8]:
        print(f"\n{'─' * 50}")
        print(f"Test: Concurrency = {concurrency}")
        rps = run_benchmark(concurrency)
        if rps is None:
            sys.exit(1)
        results[concurrency] = rps

    # Summary
    print("\n" + "=" * 60)
    print("                    SUMMARY")
    print("=" * 60)
    print(f"{'Concurrency':<15} {'Requests/sec':<15}")
    print("-" * 30)
    for conc, rps in results.items():
        print(f"{conc:<15} {rps:<15.2f}")
    print("=" * 60)

if __name__ == "__main__":
    main()
