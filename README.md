# esphttpd
A high(ish) performance webserver for the ESP32 with routing, websockets and filesystem support

## Features

- Support for HTTP and multithreaded Websockets
- File serving using any VFS mounted filesystem
- Basic routing and wilcard route support
- Fast file upload (~1MB/s max)

## How to use


Clone this component to your [ESP-IDF](https://github.com/espressif/esp-idf) v3.1.2 or later project (as a submodule): 
```
git submodule add https://github.com/funkBuild/esphttpd.git components/esphttpd
```
