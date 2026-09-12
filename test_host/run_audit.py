#!/usr/bin/env python3
"""Native sanitizer checks using production radix code and raw lifecycle functions."""
import os, subprocess, sys, tempfile
from pathlib import Path
here=Path(__file__).resolve().parent
sys.path.insert(0,str(here.parents[2]/'tools/host_test'))
sys.dont_write_bytecode = True
from extract_c_functions import extract
with tempfile.TemporaryDirectory(prefix='httpd-audit-') as d:
 p=Path(d)
 (p/'esp_log.h').write_text('#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGD(...) ((void)0)\n')
 (p/'raw.inc').write_text(extract(here.parent/'src/esphttpd.c',['file_io_worker_start','file_io_worker_stop']))
 for test in ['test_radix_audit','test_raw_lifecycle']:
  subprocess.run(['gcc','-std=gnu11','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+d,'-I'+str(here.parent/'include'),str(here/(test+'.c')),'-o',str(p/test)],check=True)
  subprocess.run([str(p/test)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
