#!/usr/bin/env python3
"""
ESP HTTP Server Unity Tests Runner for QEMU ESP32S3
Cross-platform test runner (Windows/Linux/macOS)
"""

import os
import sys
import subprocess
import time
import re
from pathlib import Path

# ANSI color codes
class Colors:
    RED = '\033[91m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def print_color(msg, color):
    """Print colored message"""
    print(f"{color}{msg}{Colors.ENDC}")

def check_environment():
    """Check if ESP-IDF environment is set up"""
    idf_path = os.environ.get('IDF_PATH')
    if not idf_path:
        print_color("Error: IDF_PATH not set. Please source ESP-IDF environment.", Colors.RED)
        print("Run: . /path/to/esp-idf/export.sh")
        return False

    print_color(f"Using ESP-IDF at: {idf_path}", Colors.GREEN)

    # Check IDF version
    try:
        result = subprocess.run(['idf.py', '--version'], capture_output=True, text=True)
        version = result.stdout.strip()
        print(f"ESP-IDF version: {version}")

        if 'v5' not in version:
            print_color("Warning: ESP-IDF v5.x recommended for QEMU support", Colors.YELLOW)
    except Exception as e:
        print_color(f"Failed to check IDF version: {e}", Colors.YELLOW)

    return True

def build_test_app():
    """Build the test application"""
    print_color("\n=== Building Test Application ===", Colors.BLUE)

    # Set target
    print("Setting target to ESP32S3...")
    result = subprocess.run(['idf.py', 'set-target', 'esp32s3'], capture_output=True)
    if result.returncode != 0:
        print_color("Failed to set target", Colors.RED)
        return False

    # Clean build
    print("Cleaning build directory...")
    subprocess.run(['idf.py', 'fullclean'], capture_output=True)

    # Build
    print("Building application...")
    result = subprocess.run(['idf.py', 'build'])
    if result.returncode != 0:
        print_color("Build failed!", Colors.RED)
        return False

    print_color("Build successful!", Colors.GREEN)
    return True

def create_qemu_image():
    """Create merged flash image for QEMU"""
    print_color("\n=== Creating QEMU Flash Image ===", Colors.BLUE)

    build_dir = Path("build")
    if not build_dir.exists():
        print_color("Build directory not found!", Colors.RED)
        return False

    # Create merge command
    merge_cmd = [
        'esptool.py',
        '--chip', 'esp32s3',
        'merge_bin',
        '-o', 'build/qemu_flash.bin',
        '--flash_mode', 'dio',
        '--flash_size', '4MB',
        '0x0', 'build/bootloader/bootloader.bin',
        '0x8000', 'build/partition_table/partition-table.bin',
        '0x10000', 'build/esphttpd_test_app.bin'
    ]

    print("Merging binaries...")
    result = subprocess.run(merge_cmd, capture_output=True)
    if result.returncode != 0:
        print_color(f"Failed to merge binaries: {result.stderr.decode()}", Colors.RED)
        return False

    print_color("Flash image created successfully!", Colors.GREEN)
    return True

def run_qemu_tests(timeout=30):
    """Run tests in QEMU"""
    print_color(f"\n=== Running Tests in QEMU (timeout: {timeout}s) ===", Colors.BLUE)

    # Check if QEMU is available
    qemu_cmd = 'qemu-system-xtensa'
    if sys.platform == 'win32':
        qemu_cmd += '.exe'

    # Build QEMU command
    qemu_args = [
        qemu_cmd,
        '-machine', 'esp32s3',
        '-drive', 'file=build/qemu_flash.bin,format=raw,if=mtd',
        '-serial', 'stdio',
        '-display', 'none'
    ]

    # Run QEMU with timeout
    try:
        print("Starting QEMU ESP32S3...")
        print("=" * 50)

        process = subprocess.Popen(
            qemu_args,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            universal_newlines=True
        )

        output = []
        test_complete = False
        test_passed = False
        start_time = time.time()

        # Read output line by line
        while True:
            if time.time() - start_time > timeout:
                print_color(f"\n⚠ Timeout after {timeout} seconds", Colors.YELLOW)
                process.terminate()
                break

            line = process.stdout.readline()
            if not line:
                break

            # Print output in real-time
            print(line, end='')
            output.append(line)

            # Check for test completion
            if "QEMU_TEST_COMPLETE: PASS" in line:
                test_complete = True
                test_passed = True
                process.terminate()
                break
            elif "QEMU_TEST_COMPLETE: FAIL" in line:
                test_complete = True
                test_passed = False
                process.terminate()
                break

        # Save output to file
        with open('qemu_output.txt', 'w') as f:
            f.writelines(output)

        # Parse and display results
        print("\n" + "=" * 50)
        if test_complete:
            if test_passed:
                print_color("✓ All tests passed!", Colors.GREEN)
                return True
            else:
                print_color("✗ Some tests failed!", Colors.RED)
                # Show failed tests
                print_color("\nFailed tests:", Colors.RED)
                for line in output:
                    if "FAIL" in line and "QEMU_TEST_COMPLETE" not in line:
                        print(f"  {line.strip()}")
                return False
        else:
            print_color("⚠ Tests did not complete", Colors.YELLOW)
            print("Check qemu_output.txt for details")
            return False

    except FileNotFoundError:
        print_color(f"Error: QEMU not found ({qemu_cmd})", Colors.RED)
        print("Please install QEMU for ESP32:")
        print("  https://github.com/espressif/qemu/wiki")
        return False
    except Exception as e:
        print_color(f"Error running QEMU: {e}", Colors.RED)
        return False

def parse_unity_output(output_file):
    """Parse Unity test output for summary"""
    if not os.path.exists(output_file):
        return

    with open(output_file, 'r') as f:
        lines = f.readlines()

    # Look for Unity summary
    for line in lines:
        if "Tests run:" in line or "TESTS RUN:" in line:
            print_color(f"\n{line.strip()}", Colors.BLUE)
            break

def main():
    """Main test runner"""
    print_color("ESP HTTP Server Unity Tests - QEMU Runner", Colors.BOLD)
    print("=" * 50)

    # Check environment
    if not check_environment():
        return 1

    # Build application
    if not build_test_app():
        return 1

    # Create QEMU image
    if not create_qemu_image():
        return 1

    # Run tests
    success = run_qemu_tests(timeout=30)

    # Parse output
    parse_unity_output('qemu_output.txt')

    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())