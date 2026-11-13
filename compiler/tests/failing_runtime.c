/* Same runtime/target as native builds, with a bounded fault allocator.
   Default zero forces initial failure; limit two exercises partial success. */
#include <stddef.h>
#include <stdlib.h>
#ifndef VX_TEST_ALLOC_LIMIT
#define VX_TEST_ALLOC_LIMIT 0
#endif
#ifndef VX_TEST_FAIL_AT
#define VX_TEST_FAIL_AT 0
#endif
static unsigned test_live;
static void check_freed(void) { if(test_live) abort(); }
static void *fail_allocation(size_t bytes) {
    static int started;
    static unsigned attempts;
    if(!started) { atexit(check_freed); started=1; }
    ++attempts;
    if(VX_TEST_FAIL_AT&&attempts==VX_TEST_FAIL_AT) return NULL;
    if(test_live==VX_TEST_ALLOC_LIMIT) return NULL;
    void *p=malloc(bytes); if(p) ++test_live; return p;
}
static void test_free(void *p) { if(!test_live) abort(); --test_live; free(p); }
#define VX_HEAP_ALLOC fail_allocation
#define VX_HEAP_FREE test_free
#include "../runtime/native.c"
