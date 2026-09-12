#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int allocations, fail_at;
static void *fault_malloc(size_t n) { return ++allocations == fail_at ? NULL : malloc(n); }
static void *fault_calloc(size_t n, size_t s) { return ++allocations == fail_at ? NULL : calloc(n,s); }
static void *fault_realloc(void *p,size_t n) { return ++allocations == fail_at ? NULL : realloc(p,n); }
#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#include "../src/radix_tree.c"
#undef malloc
#undef calloc
#undef realloc
static httpd_err_t handler(httpd_req_t *r) {(void)r;return HTTPD_OK;}
static httpd_err_t ws_handler(httpd_ws_t *w,httpd_ws_event_t *e) {(void)w;(void)e;return HTTPD_OK;}
static httpd_err_t middleware(httpd_req_t *r,httpd_next_t n) {(void)r;(void)n;return HTTPD_OK;}
int main(void) {
    for (int ws=0;ws<2;ws++) for(int fail=1;fail<=8;fail++) {
        fail_at=0;radix_tree_t *tree=radix_tree_create();assert(tree);
        allocations=0;fail_at=fail;
        httpd_middleware_t mw=middleware;
        httpd_err_t err=ws ? radix_insert_ws(tree,"/oom",ws_handler,(void *)1,0,&mw,1)
                          : radix_insert(tree,"/oom",HTTP_GET,handler,(void *)1,&mw,1);
        fail_at=0;radix_match_t match;
        radix_lookup(tree,"/oom",HTTP_GET,ws,&match,NULL,NULL);
        assert(match.matched==(err==HTTPD_OK));
        radix_tree_destroy(tree);
    }
    puts("PASS HTTP and WebSocket registration remains unpublished on every allocation failure");
    radix_tree_t *tree=radix_tree_create();
    char path[2*(RADIX_MAX_DEPTH+1)+1];
    for(unsigned i=0;i<RADIX_MAX_DEPTH+1;i++) {path[2*i]='/';path[2*i+1]='a';}path[sizeof(path)-1]=0;
    assert(radix_insert(tree,path,HTTP_GET,handler,NULL,NULL,0)==HTTPD_ERR_INVALID_ARG);
    assert(radix_insert_ws(tree,path,ws_handler,NULL,0,NULL,0)==HTTPD_ERR_INVALID_ARG);
    assert(tree->node_count==1);
    path[2*RADIX_MAX_DEPTH]=0;
    assert(radix_insert(tree,path,HTTP_GET,handler,NULL,NULL,0)==HTTPD_OK);
    radix_match_t match;radix_lookup(tree,path,HTTP_GET,false,&match,NULL,NULL);assert(match.matched);
    radix_tree_destroy(tree);
    assert(!radix_node_create("x",SIZE_MAX,NODE_STATIC));
    puts("PASS route depth boundary and segment allocation overflow");
    return 0;
}
