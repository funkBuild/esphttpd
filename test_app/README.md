# ESP HTTP Server Unity Test App for QEMU ESP32S3

This test application runs Unity framework tests for the refactored esphttpd server on QEMU ESP32S3 emulator, allowing fast testing without physical hardware.

## Features

- ✅ Unity test framework integration
- ✅ Runs on QEMU ESP32S3 emulator
- ✅ No physical hardware required
- ✅ Automated test execution
- ✅ Cross-platform support (Linux/macOS/Windows)
- ✅ Comprehensive test coverage:
  - HTTP parser (streaming, chunked, WebSocket upgrade)
  - WebSocket frame processing (masked/unmasked, fragmented)
  - Route matching (wildcards, priorities, methods)
  - Connection management (pool, states, packed structures)

## Prerequisites

### 1. ESP-IDF v5.0+

Install ESP-IDF with QEMU support:

```bash
# Clone ESP-IDF
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf
./install.sh esp32s3

# Source environment
. ~/esp-idf/export.sh
```

### 2. QEMU for ESP32

Install QEMU with ESP32 support:

#### Linux (Ubuntu/Debian)
```bash
# Install dependencies
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv
sudo apt-get install cmake ninja-build ccache libffi-dev libssl-dev dfu-util
sudo apt-get install libusb-1.0-0 libglib2.0-dev libpixman-1-dev

# Build QEMU for ESP32
git clone https://github.com/espressif/qemu.git
cd qemu
./configure --target-list=xtensa-softmmu --enable-debug --enable-sanitizers
make -j8
sudo make install
```

#### macOS
```bash
# Using Homebrew
brew tap espressif/tap
brew install espressif/tap/qemu-esp32
```

#### Windows
Download pre-built binaries from:
https://github.com/espressif/qemu/releases

## Project Structure

```
test_app/
├── main/
│   ├── CMakeLists.txt         # Component configuration
│   ├── test_app_main.c        # Main test runner
│   ├── test_http_parser.c     # HTTP parser tests
│   ├── test_websocket_frame.c # WebSocket tests
│   ├── test_routing.c         # Router tests
│   └── test_connection.c      # Connection management tests
├── CMakeLists.txt             # Project configuration
├── sdkconfig.defaults         # Default ESP32S3 config
├── run_qemu_tests.sh         # Bash test runner
├── run_tests.py              # Python test runner
└── README.md                 # This file
```

## Running Tests

### Method 1: Using Shell Script (Linux/macOS)

```bash
cd test_app
chmod +x run_qemu_tests.sh
./run_qemu_tests.sh
```

### Method 2: Using Python Script (Cross-platform)

```bash
cd test_app
python3 run_tests.py
```

### Method 3: Manual Steps

```bash
# 1. Set ESP-IDF environment
. ~/esp-idf/export.sh

# 2. Configure for ESP32S3
cd test_app
idf.py set-target esp32s3

# 3. Build the test app
idf.py build

# 4. Create QEMU flash image
esptool.py --chip esp32s3 merge_bin \
    -o build/qemu_flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x10000 build/esphttpd_test_app.bin

# 5. Run in QEMU
qemu-system-xtensa \
    -machine esp32s3 \
    -drive file=build/qemu_flash.bin,format=raw,if=mtd \
    -serial stdio \
    -display none
```

## Expected Output

Successful test run:
```
ESP HTTP Server Unity Tests - QEMU Runner
=========================================
Starting esphttpd Unity tests on ESP32S3 QEMU
Running HTTP Parser tests...
Running WebSocket Frame tests...
Running Routing tests...
Running Connection tests...
-----------------------
96 Tests 0 Failures 0 Ignored
All tests passed!
QEMU_TEST_COMPLETE: PASS
```

Failed test example:
```
test_http_parser.c:45:test_parse_post_with_content_length:FAIL: Expected 123 Was 0
-----------------------
96 Tests 1 Failures 0 Ignored
QEMU_TEST_COMPLETE: FAIL
```

## Test Coverage

### HTTP Parser Tests (10 tests)
- GET/POST/OPTIONS request parsing
- Chunked request reception
- WebSocket upgrade detection
- Content-Length handling
- Keep-Alive parsing
- Invalid request handling

### WebSocket Frame Tests (9 tests)
- Unmasked/masked frames
- Extended payload lengths (16-bit)
- Fragmented frames
- Control frames (close, ping, pong)
- Frame building
- XOR masking/unmasking

### Routing Tests (9 tests)
- Exact route matching
- Wildcard patterns (`*`, `?`)
- Route priorities
- Multi-method routes
- WebSocket routes
- Query parameter handling

### Connection Tests (12 tests)
- Connection pool management
- State transitions
- Active/inactive tracking
- WebSocket state fields
- Structure packing verification

## Debugging Tests

### Enable Verbose Logging

Edit `sdkconfig.defaults`:
```
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
CONFIG_UNITY_ENABLE_FIXTURE=y
```

### Running Specific Tests

Modify `test_app_main.c` to run only specific test suites:
```c
void app_main(void)
{
    UNITY_BEGIN();

    // Run only parser tests
    test_http_parser_run();

    UNITY_END();
}
```

### GDB Debugging with QEMU

```bash
# Terminal 1: Start QEMU with GDB server
qemu-system-xtensa \
    -machine esp32s3 \
    -drive file=build/qemu_flash.bin,format=raw,if=mtd \
    -serial stdio \
    -display none \
    -s -S  # Wait for GDB connection

# Terminal 2: Connect with GDB
xtensa-esp32s3-elf-gdb build/esphttpd_test_app.elf
(gdb) target remote :1234
(gdb) break test_parse_get_request
(gdb) continue
```

## Adding New Tests

1. Create new test file in `main/`:
```c
// main/test_new_feature.c
#include "unity.h"

static void test_feature_x(void) {
    TEST_ASSERT_EQUAL(expected, actual);
}

void test_new_feature_run(void) {
    RUN_TEST(test_feature_x);
}
```

2. Add to `main/CMakeLists.txt`:
```cmake
SRCS
    ...
    "test_new_feature.c"
```

3. Call from `test_app_main.c`:
```c
void test_new_feature_run(void);

void app_main(void) {
    ...
    test_new_feature_run();
    ...
}
```

## Continuous Integration

### GitHub Actions Example

```yaml
name: ESP32S3 QEMU Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v2

    - name: Setup ESP-IDF
      uses: espressif/setup-esp-idf@v1
      with:
        esp-idf-version: v5.2.1
        targets: esp32s3

    - name: Install QEMU
      run: |
        sudo apt-get update
        sudo apt-get install -y qemu-system-misc

    - name: Run Tests
      run: |
        cd test_app
        python3 run_tests.py
```

## Troubleshooting

### QEMU Not Found
```
Error: qemu-system-xtensa not found
```
**Solution**: Install QEMU for ESP32 (see Prerequisites)

### IDF_PATH Not Set
```
Error: IDF_PATH not set
```
**Solution**: Source ESP-IDF environment: `. ~/esp-idf/export.sh`

### Build Fails
```
CMake Error: Unknown CMake command "idf_component_register"
```
**Solution**: Ensure you're using ESP-IDF's idf.py, not system cmake

### QEMU Hangs
- Check flash image was created correctly
- Verify ESP32S3 target is set
- Try increasing timeout in run_tests.py

### Tests Timeout
- Default timeout is 30 seconds
- Increase if running on slow systems:
  ```python
  run_qemu_tests(timeout=60)  # 60 seconds
  ```

## Performance

Typical execution times:
- Build: 20-30 seconds (first build)
- Build: 2-5 seconds (incremental)
- QEMU startup: 1-2 seconds
- Test execution: 3-5 seconds
- **Total**: ~10 seconds for full test run

## License

Same as esphttpd project - see parent LICENSE file.