/* Exercise pool growth, fragmented page reuse and live-allocation isolation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int fail_metadata, fail_pages;
static void *test_realloc(void *ptr, size_t size)
{
    if (fail_metadata) { fail_metadata = 0; return NULL; }
    return realloc(ptr, size);
}
static void *test_aligned_alloc(size_t alignment, size_t size)
{
    if (fail_pages) { fail_pages = 0; return NULL; }
    return aligned_alloc(alignment, size);
}
#define realloc test_realloc
#define aligned_alloc test_aligned_alloc
#include "../../dlls/ntdll/unix/horizon_pool.h"
#undef realloc
#undef aligned_alloc

#define TEST_ARENAS 512

struct object { void *link; unsigned long long value[6]; };
static struct object storage[4];
static struct horizon_object_pool objects = { storage, NULL, sizeof(storage[0]), 4, 0 };
static struct horizon_page_pool pool;
static unsigned int random_state = 42;
static unsigned int random_next(void) { return random_state = random_state * 1664525 + 1013904223; }

int main(void)
{
    struct object *items[5];
    void *whole[TEST_ARENAS];
    struct { unsigned char *ptr; size_t size; unsigned char tag; } live[128] = {0};
    unsigned int i, j, round;
    size_t bytes;
    for (i = 0; i < 5; i++)
    {
        items[i] = horizon_object_alloc(&objects);
        assert(items[i]);
        items[i]->value[0] = 123;
    }
    assert((uintptr_t)items[4] < (uintptr_t)storage || (uintptr_t)items[4] >= (uintptr_t)(storage + 4));
    horizon_object_free(&objects, items[2]);
    assert(horizon_object_alloc(&objects) == items[2] && !items[2]->value[0]);
    for (i = 0; i < 5; i++) horizon_object_free(&objects, items[i]);
    assert(!horizon_pages_alloc(&pool, 0));
    assert(!horizon_pages_alloc(&pool, 4097));
    assert(!horizon_pages_alloc(&pool, (HORIZON_POOL_PAGES + 1) * HORIZON_POOL_PAGE));
    bytes = HORIZON_POOL_ARENA;
    for (i = 0; i < TEST_ARENAS; i++)
    {
        whole[i] = horizon_pages_alloc(&pool, bytes);
        /* One arena is one kernel memory block, and shares it with nothing:
         * aliasing a page of it unmaps whatever else the block holds. */
        assert(whole[i] && !((uintptr_t)whole[i] % HORIZON_POOL_ARENA));
        ((unsigned char *)whole[i])[bytes - 1] = (unsigned char)(i + 1);
    }
    assert(pool.active_arenas == TEST_ARENAS && pool.peak_arenas == TEST_ARENAS);
    assert(pool.capacity == TEST_ARENAS);
    fail_metadata = 1;
    assert(!horizon_pages_alloc(&pool, HORIZON_POOL_PAGE));
    assert(pool.capacity == TEST_ARENAS && pool.active_arenas == TEST_ARENAS);
    fail_pages = 1;
    assert(!horizon_pages_alloc(&pool, HORIZON_POOL_PAGE));
    assert(pool.capacity > TEST_ARENAS && pool.active_arenas == TEST_ARENAS);
    /* Growing the metadata leaves all existing page allocations in place. */
    {
        void *one = horizon_pages_alloc_any(&pool, HORIZON_POOL_PAGE);
        void *two = horizon_pages_alloc_any(&pool, HORIZON_POOL_PAGE);
        void *large = horizon_pages_alloc_any(&pool, HORIZON_POOL_ARENA + HORIZON_POOL_PAGE);
        assert(one && !((uintptr_t)one % HORIZON_POOL_ARENA));
        assert(two == (char *)one + HORIZON_POOL_PAGE);
        assert(pool.active_arenas == TEST_ARENAS + 1);
        assert(large && !((uintptr_t)large % HORIZON_POOL_ARENA));
        assert(horizon_pages_free(&pool, one, HORIZON_POOL_PAGE));
        assert(horizon_pages_free(&pool, two, HORIZON_POOL_PAGE));
        assert(pool.blocks == 1 && !pool.shared);
        free(large);
    }
    /* Release and reuse a middle arena while every other arena remains live. */
    assert(horizon_pages_free(&pool, whole[7], bytes));
    assert(pool.active_arenas == TEST_ARENAS - 1);
    whole[7] = horizon_pages_alloc(&pool, bytes);
    assert(whole[7] && pool.active_arenas == TEST_ARENAS);
    ((unsigned char *)whole[7])[bytes - 1] = 8;
    for (i = 0; i < TEST_ARENAS; i++)
    {
        assert(((unsigned char *)whole[i])[bytes - 1] == (unsigned char)(i + 1));
        assert(horizon_pages_free(&pool, whole[i], bytes));
    }
    for (round = 0; round < 20000; round++)
    {
        i = (random_next() >> 8) % 128;
        if (live[i].ptr)
        {
            for (j = 0; j < live[i].size; j += HORIZON_POOL_PAGE)
                assert(live[i].ptr[j] == live[i].tag);
            assert(horizon_pages_free(&pool, live[i].ptr, live[i].size));
            live[i].ptr = NULL;
        }
        else
        {
            live[i].size = (1 + (random_next() >> 8) % 128) * HORIZON_POOL_PAGE;
            live[i].ptr = horizon_pages_alloc(&pool, live[i].size);
            assert(live[i].ptr);
            assert(!((uintptr_t)live[i].ptr % HORIZON_POOL_PAGE));
            for (j = 0; j < 128; j++) if (j != i && live[j].ptr)
                assert((uintptr_t)live[i].ptr + live[i].size <= (uintptr_t)live[j].ptr ||
                       (uintptr_t)live[j].ptr + live[j].size <= (uintptr_t)live[i].ptr);
            live[i].tag = 1 + i;
            memset(live[i].ptr, live[i].tag, live[i].size);
        }
    }
    for (i = 0; i < 128; i++) if (live[i].ptr)
        assert(horizon_pages_free(&pool, live[i].ptr, live[i].size));
    assert(pool.active_arenas == HORIZON_POOL_RETAIN_EMPTY && pool.reclaims >= TEST_ARENAS - HORIZON_POOL_RETAIN_EMPTY);
    j = 0;
    for (i = 0; i < pool.capacity; i++)
    {
        if (!pool.arenas[i].memory) continue;
        j++;
        assert(pool.arenas[i].free_pages == HORIZON_POOL_PAGES);
        for (unsigned int page = 0; page < HORIZON_POOL_PAGES; page++) assert(!pool.arenas[i].used[page]);
    }
    assert(j == HORIZON_POOL_RETAIN_EMPTY);
    whole[0] = horizon_pages_alloc(&pool, HORIZON_POOL_PAGE);
    assert(whole[0]);
    memset(whole[0], 0xa5, HORIZON_POOL_PAGE);
    assert(horizon_pages_trim(&pool) == (HORIZON_POOL_RETAIN_EMPTY - 1) * HORIZON_POOL_ARENA);
    assert(pool.active_arenas == 1);
    for (i = 0; i < HORIZON_POOL_PAGE; i++) assert(((unsigned char *)whole[0])[i] == 0xa5);
    assert(horizon_pages_free(&pool, whole[0], HORIZON_POOL_PAGE));
    assert(horizon_pages_trim(&pool) == HORIZON_POOL_ARENA);
    assert(!pool.active_arenas && !pool.capacity && !pool.arenas);
    whole[0] = horizon_pages_alloc(&pool, HORIZON_POOL_PAGE);
    assert(whole[0] && pool.active_arenas == 1);
    assert(horizon_pages_free(&pool, whole[0], HORIZON_POOL_PAGE));
    assert(horizon_pages_trim(&pool) == HORIZON_POOL_ARENA);
    assert(!horizon_pages_alloc_dedicated(&pool, SIZE_MAX & ~(size_t)(HORIZON_POOL_PAGE - 1)));
    puts("Mapping pools: growth failures, packed overflow, reclamation, alignment and 20000 fragmented allocation cycles passed");
    return 0;
}
