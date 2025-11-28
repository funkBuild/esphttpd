# Performance Baseline Results

**Date**: 2025-11-28
**Platform**: ESP32S3 QEMU @ 160MHz
**Commit**: f7cb520-dirty
**ESP-IDF**: v5.5.1

## Benchmark Results

### HTTP Parser
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| http_parse_method | 73 | 30000 | 2201 |
| http_identify_header | 121 | 40000 | 4879 |
| http_parse_content_length | 56 | 30000 | 1685 |
| http_parse_request | 2185 | 1000 | 2185 |
| http_parse_url_params | 145 | 10000 | 1456 |

### Radix Tree Router
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| radix_lookup_static | 623 | 10000 | 6234 |
| radix_lookup_param | 468 | 10000 | 4687 |
| radix_lookup_deep | 884 | 10000 | 8847 |
| radix_lookup_miss | 514 | 10000 | 5141 |
| radix_insert (3 routes) | 2030924 | 1000 | 2030924 |

### WebSocket Frame
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| ws_mask_payload_128 | 101 | 10000 | 1017 |
| ws_build_frame_header | 44 | 30000 | 1344 |
| ws_process_frame_64 | 363 | 5000 | 1815 |

### Template Engine
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| template_process_plain | 469 | 5000 | 2346 |
| template_process_3vars | 1918 | 5000 | 9592 |

### Connection Pool
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| connection_find | 102 | 30000 | 3064 |
| connection_get_index | 45 | 30000 | 1363 |
| connection_count_active | 90 | 10000 | 906 |

### Utility Functions
| Benchmark | ns/op | Iterations | Total (us) |
|-----------|-------|------------|------------|
| httpd_get_mime_type | 418 | 40000 | 16759 |
| httpd_status_text | 41 | 40000 | 1663 |

## Raw Output

```
PERF: http_parse_method: 2201 us total, 73 ns/op (30000 iterations)
PERF: http_identify_header: 4879 us total, 121 ns/op (40000 iterations)
PERF: http_parse_content_length: 1685 us total, 56 ns/op (30000 iterations)
PERF: http_parse_request: 2185 us total, 2185 ns/op (1000 iterations)
PERF: http_parse_url_params: 1456 us total, 145 ns/op (10000 iterations)
PERF: radix_lookup_static: 6234 us total, 623 ns/op (10000 iterations)
PERF: radix_lookup_param: 4687 us total, 468 ns/op (10000 iterations)
PERF: radix_lookup_deep: 8847 us total, 884 ns/op (10000 iterations)
PERF: radix_lookup_miss: 5141 us total, 514 ns/op (10000 iterations)
PERF: radix_insert (3 routes): 2030924 us total, 2030924 ns/op (1000 iterations)
PERF: ws_mask_payload_128: 1017 us total, 101 ns/op (10000 iterations)
PERF: ws_build_frame_header: 1344 us total, 44 ns/op (30000 iterations)
PERF: ws_process_frame_64: 1815 us total, 363 ns/op (5000 iterations)
PERF: template_process_plain: 2346 us total, 469 ns/op (5000 iterations)
PERF: template_process_3vars: 9592 us total, 1918 ns/op (5000 iterations)
PERF: connection_find: 3064 us total, 102 ns/op (30000 iterations)
PERF: connection_get_index: 1363 us total, 45 ns/op (30000 iterations)
PERF: connection_count_active: 906 us total, 90 ns/op (10000 iterations)
PERF: httpd_get_mime_type: 16759 us total, 418 ns/op (40000 iterations)
PERF: httpd_status_text: 1663 us total, 41 ns/op (40000 iterations)
```

## Notes

- QEMU timing may differ from real ESP32S3 hardware
- `radix_insert` includes tree creation and destruction overhead per iteration
- All benchmarks include cache warmup before timing
- Template benchmarks use `{{variable}}` delimiter syntax
