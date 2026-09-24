#!/usr/bin/env bash
# Build the benchmark image and run it under QEMU with -icount (deterministic).
# Usage: bench/run.sh [filter-substring]   -> BENCH lines on stdout
# BENCH_OUT=<file> keeps the full serial log. BENCH_ELF_COPY=<file> saves the ELF.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null 2>&1
cd "$HERE"
FILTER="${1:-}"
idf.py -B build -D SDKCONFIG="$HERE/build/sdkconfig" build >build.log 2>&1 \
  || { tail -30 build.log; exit 1; }
cd build
esptool.py --chip esp32s3 merge_bin -o flash.bin --flash_mode dio --flash_size 4MB --fill-flash-size 4MB \
  0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 esphttpd_bench.bin >/dev/null
EFUSE_TOOL="$HERE/../../../tools/qemu/make_efuse.py"
python3 "$EFUSE_TOOL" efuse.bin >/dev/null
Q=$(ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa | sort | tail -1)
OUT="${BENCH_OUT:-$HERE/build/bench_serial.log}"
rm -f "$OUT"
timeout 900 "$Q" -M esp32s3 -icount shift=2 -drive file=flash.bin,if=mtd,format=raw \
  -drive file=efuse.bin,if=none,format=raw,id=efuse -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
  -serial file:"$OUT" -display none -monitor none -nic user,model=open_eth -no-reboot &
P=$!
for _ in $(seq 1 900); do
  kill -0 $P 2>/dev/null || break
  grep -a -q "BENCH_DONE" "$OUT" 2>/dev/null && break
  sleep 1
done
kill -9 $P 2>/dev/null || true
[ -n "${BENCH_ELF_COPY:-}" ] && cp esphttpd_bench.elf "$BENCH_ELF_COPY"
grep -a "^BENCH" "$OUT" | grep -a -- "${FILTER}" || true
