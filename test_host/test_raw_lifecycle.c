#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#define MAX_CONNECTIONS 8
#define SEND_BUFFER_SIZE 512
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define HTTPD_LOCK_TCPIP() ((void)0)
#define HTTPD_UNLOCK_TCPIP() ((void)0)
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
typedef struct { uint8_t pool_index; uint16_t generation; int file_fd; size_t max_len; } file_io_request_t;
static QueueHandle_t s_file_io_request_queue;
static _Atomic(TaskHandle_t) s_file_io_task;
static atomic_bool s_file_io_stopping;
static bool s_file_io_stop_queued;
static int fail_queue, fail_bounce, fail_task, queue_full, queue_count, sends;
static unsigned ticks;
static bool finish_worker;
static void *worker_arg;
static void *xQueueCreate(int n,size_t size) {(void)n;(void)size;if(fail_queue)return NULL;queue_count++;return calloc(1,1);}
static void vQueueDelete(void *p) {assert(p);queue_count--;free(p);}
static int xQueueSend(void *q,const file_io_request_t *r,unsigned wait) {(void)wait;assert(q&&r->file_fd==-1);sends++;return !queue_full;}
static void file_io_worker_task(void *arg) {(void)arg;}
static int xTaskCreate(void (*fn)(void *),const char *name,int stack,void *arg,int priority,TaskHandle_t *out) {
    (void)fn;(void)name;(void)stack;(void)priority;
    if(fail_task)return 0;worker_arg=arg;*out=(void *)1;return pdPASS;
}
static unsigned xTaskGetTickCount(void) {return ticks;}
static void vTaskDelay(unsigned n) {ticks+=n;if(finish_worker){free(worker_arg);worker_arg=NULL;s_file_io_task=NULL;}}
static void *fault_malloc(size_t n) {return fail_bounce?NULL:malloc(n);}
#define malloc fault_malloc
#include "raw.inc"
#undef malloc
int main(void) {
    for(int i=0;i<3;i++) {
        fail_queue=i==0;fail_bounce=i==1;fail_task=i==2;
        file_io_worker_start();assert(!s_file_io_request_queue&&!s_file_io_task&&queue_count==0);
    }
    puts("PASS raw file worker queue, buffer and task allocation rollback");
    fail_queue=fail_bounce=fail_task=0;
    file_io_worker_start();assert(s_file_io_task&&queue_count==1);
    queue_full=1;assert(!file_io_worker_stop()&&queue_count==1&&!s_file_io_stop_queued);
    puts("PASS raw full stop queue retains live worker resources");
    queue_full=0;assert(!file_io_worker_stop()&&queue_count==1&&s_file_io_stop_queued);
    int prior_sends=sends;finish_worker=true;
    assert(file_io_worker_stop()&&sends==prior_sends&&!s_file_io_request_queue&&queue_count==0);
    assert(file_io_worker_stop());
    puts("PASS raw worker join timeout, retry and idempotent cleanup");
}
