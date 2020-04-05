
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */

#include <ngx_config.h>
#include <ngx_core.h>


#define NGX_SLAB_PAGE_MASK   3
#define NGX_SLAB_PAGE        0
#define NGX_SLAB_BIG         1
#define NGX_SLAB_EXACT       2
#define NGX_SLAB_SMALL       3

#if (NGX_PTR_SIZE == 4)

#define NGX_SLAB_PAGE_FREE   0
#define NGX_SLAB_PAGE_BUSY   0xffffffff
#define NGX_SLAB_PAGE_START  0x80000000

#define NGX_SLAB_SHIFT_MASK  0x0000000f
#define NGX_SLAB_MAP_MASK    0xffff0000
#define NGX_SLAB_MAP_SHIFT   16

#define NGX_SLAB_BUSY        0xffffffff

#else /* (NGX_PTR_SIZE == 8) */

#define NGX_SLAB_PAGE_FREE   0
#define NGX_SLAB_PAGE_BUSY   0xffffffffffffffff
#define NGX_SLAB_PAGE_START  0x8000000000000000

#define NGX_SLAB_SHIFT_MASK  0x000000000000000f
#define NGX_SLAB_MAP_MASK    0xffffffff00000000
#define NGX_SLAB_MAP_SHIFT   32

#define NGX_SLAB_BUSY        0xffffffffffffffff

#endif


#define ngx_slab_slots(pool)                                                  \
    (ngx_slab_page_t *) ((u_char *) (pool) + sizeof(ngx_slab_pool_t))

#define ngx_slab_page_type(page)   ((page)->prev & NGX_SLAB_PAGE_MASK)

#define ngx_slab_page_prev(page)                                              \
    (ngx_slab_page_t *) ((page)->prev & ~NGX_SLAB_PAGE_MASK)

#define ngx_slab_page_addr(pool, page)                                        \
    ((((page) - (pool)->pages) << ngx_pagesize_shift)                         \
     + (uintptr_t) (pool)->start)


#if (NGX_DEBUG_MALLOC)

#define ngx_slab_junk(p, size)     ngx_memset(p, 0xA5, size)

#elif (NGX_HAVE_DEBUG_MALLOC)

#define ngx_slab_junk(p, size)                                                \
    if (ngx_debug_malloc)          ngx_memset(p, 0xA5, size)

#else

#define ngx_slab_junk(p, size)

#endif

static ngx_slab_page_t *ngx_slab_alloc_pages(ngx_slab_pool_t *pool,
    ngx_uint_t pages);
static void ngx_slab_free_pages(ngx_slab_pool_t *pool, ngx_slab_page_t *page,
    ngx_uint_t pages);
static void ngx_slab_error(ngx_slab_pool_t *pool, ngx_uint_t level,
    char *text);


static ngx_uint_t  ngx_slab_max_size;
static ngx_uint_t  ngx_slab_exact_size;
static ngx_uint_t  ngx_slab_exact_shift;


void
ngx_slab_sizes_init(void)
{
    ngx_uint_t  n;

    ngx_slab_max_size = ngx_pagesize / 2;
    ngx_slab_exact_size = ngx_pagesize / (8 * sizeof(uintptr_t));
    for (n = ngx_slab_exact_size; n >>= 1; ngx_slab_exact_shift++) {
        /* void */
    }
}


void
ngx_slab_init(ngx_slab_pool_t *pool)
{
    u_char           *p;
    size_t            size;
    ngx_int_t         m;
    ngx_uint_t        i, n, pages;
    ngx_slab_page_t  *slots, *page;

    /*
     * NOTE: pool->min_shift 在 nginx_init_zone_pool 中被设置为 3
     */
    pool->min_size = (size_t) 1 << pool->min_shift;

    slots = ngx_slab_slots(pool);

    p = (u_char *) slots;
    size = pool->end - p;

    /*
     * QUESTION: 这里的 slab junk 是什么？
     */
    ngx_slab_junk(p, size);

    /*
     * NOTE: 以 4K 的页为例，ngx_pagesize_shift = 12，而 pool->min_shift = 3
     *       nginx 规定页面中能存放的最大的内存块大小为 pagesize/2，在这里就是 2K
     *       所以这里应该是 ngx_pagesize_shift - 1 - pool->min_shift + 1
     *       所以 slots 数组一共有 8 个槽
     */
    n = ngx_pagesize_shift - pool->min_shift;

    for (i = 0; i < n; i++) {
        /* only "next" is used in list head */
        slots[i].slab = 0;
        /*
         * NOTE: slot 中的 next 是一个 dummy head，如果这条链表中没有元素，那么就指向
         *       自己，
         *
         * QUESTION: 这样有什么好处？我感觉直接指向 NULL 也是 ok 的啊？
         */
        slots[i].next = &slots[i];
        slots[i].prev = 0;
    }

    /*
     * NOTE: 在 slots 之后是 stats，每个 stat 对应一个 slot
     *       这个是用来记录下每个 slot 所有的和已使用的entry(?)，内存分配的请求数以及失败
     *       次数
     */
    p += n * sizeof(ngx_slab_page_t);

    pool->stats = (ngx_slab_stat_t *) p;
    ngx_memzero(pool->stats, n * sizeof(ngx_slab_stat_t));

    /*
     * NOTE: 在 stats 之后是 pages，
     */
    p += n * sizeof(ngx_slab_stat_t);

    size -= n * (sizeof(ngx_slab_page_t) + sizeof(ngx_slab_stat_t));

    /*
     * NOTE: 每个 page 都在 pool->pages 数组中对应一个结构体
     *       所以计算 page 的数目时需要在分母加上 sizeof(ngx_slab_page_t)
     */
    pages = (ngx_uint_t) (size / (ngx_pagesize + sizeof(ngx_slab_page_t)));

    pool->pages = (ngx_slab_page_t *) p;
    ngx_memzero(pool->pages, pages * sizeof(ngx_slab_page_t));

    page = pool->pages;

    /*
     * NOTE: 所有的空闲页构成一个双向链表，pool->free 指向这个链表
     *       但是有个地方需要注意，不是每个空闲页都是这个链表里面的一个独立的节点，
     *       多个相邻的页面可能只有第一个是链表中的节点，然后该页面的 slab 表示它代表的
     *       空闲页面的个数，比如说刚刚初始化完毕，free 链表中只有一个节点，但是这个节点
     *       包含了所有的空闲页，这个是通过 page->slab = pages; 体现的
     */

    /* only "next" is used in list head */
    pool->free.slab = 0;
    pool->free.next = page;
    pool->free.prev = 0;

    page->slab = pages;
    page->next = &pool->free;
    page->prev = (uintptr_t) &pool->free;

    /*
     * NOTE: 让内存以 pagesize 对齐
     */
    pool->start = ngx_align_ptr(p + pages * sizeof(ngx_slab_page_t),
                                ngx_pagesize);

    m = pages - (pool->end - pool->start) / ngx_pagesize;
    if (m > 0) {
        pages -= m;
        page->slab = pages;
    }

    pool->last = pool->pages + pages;
    pool->pfree = pages;

    /*
     * NOTE: 这个 log_ctx 是用来
     */
    pool->log_nomem = 1;
    pool->log_ctx = &pool->zero;
    pool->zero = '\0';
}


void *
ngx_slab_alloc(ngx_slab_pool_t *pool, size_t size)
{
    void  *p;

    ngx_shmtx_lock(&pool->mutex);

    p = ngx_slab_alloc_locked(pool, size);

    ngx_shmtx_unlock(&pool->mutex);

    return p;
}


void *
ngx_slab_alloc_locked(ngx_slab_pool_t *pool, size_t size)
{
    size_t            s;
    uintptr_t         p, m, mask, *bitmap;
    ngx_uint_t        i, n, slot, shift, map;
    ngx_slab_page_t  *page, *prev, *slots;

    if (size > ngx_slab_max_size) {

        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, ngx_cycle->log, 0,
                       "slab alloc: %uz", size);

        page = ngx_slab_alloc_pages(pool, (size >> ngx_pagesize_shift)
                                          + ((size % ngx_pagesize) ? 1 : 0));
        if (page) {
            p = ngx_slab_page_addr(pool, page);

        } else {
            /*
             * NOTE: done 处会返回 (void *) p，对于 0，返回的是 NULL
             */
            p = 0;
        }

        goto done;
    }

    /*
     * NOTE: 如果需要分配的不是超过了 pagesize/2 的大块内存
     *       那么需要在 slot 数组中分配
     *       1. 找到合适的 slot 槽
     *       2.
     */

    if (size > pool->min_size) {
        shift = 1;
        for (s = size - 1; s >>= 1; shift++) { /* void */ }
        slot = shift - pool->min_shift;

    } else {
        shift = pool->min_shift;
        slot = 0;
    }

    // NOTE: 记录下内存分配请求
    pool->stats[slot].reqs++;

    ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, ngx_cycle->log, 0,
                   "slab alloc: %uz slot: %ui", size, slot);

    slots = ngx_slab_slots(pool);
    page = slots[slot].next;

    if (page->next != page) {

        /*
         * NOTE: nginx 中 slab 内存块按照其大小一共分为 4 种
         *       1. 中等内存: 如果可以直接用 page->slab 字段作为内存块是否使用的 bitmap
         *       那么 4K/(8 * sizeof(uintptr_t) = 2^6 = 64B 就是中等内存
         *       2. 小块内存: 比中等内存小的(当然最小也得 8B)，这种内存块中，ngx_slab_page_t
         *       中的 slab 字段就不足以记录下该 page 中所有内存块的使用情况，所以需要使
         *       用一部分内存来充当 bitmap
         *       3. 大块内存: 比中等内存大，但是不超过最大内存块(2KB)
         *       3. 超大内存:
         */
        if (shift < ngx_slab_exact_shift) {

            bitmap = (uintptr_t *) ngx_slab_page_addr(pool, page);

            map = (ngx_pagesize >> shift) / (8 * sizeof(uintptr_t));

            for (n = 0; n < map; n++) {

                if (bitmap[n] != NGX_SLAB_BUSY) {

                    for (m = 1, i = 0; m; m <<= 1, i++) {
                        if (bitmap[n] & m) {
                            continue;
                        }

                        // NOTE: 标记这块内存被使用了
                        bitmap[n] |= m;

                        /*
                         * NOTE: n * 8 * sizeof(uintptr_t) + i 表示的是找到的这块内存
                         *       在这个 page 里面是第几块内存
                         *       << shift 就拿到了这块内存在该 page 中的偏移量
                         */
                        i = (n * 8 * sizeof(uintptr_t) + i) << shift;

                        // NOTE: 最后返回的就是 p
                        p = (uintptr_t) bitmap + i;

                        pool->stats[slot].used++;

                        /*
                         * NOTE: 在标记这块内存为在使用状态之后，可能整个 page 都被使用
                         *       了，这个时候就需要把这个 page 从半满链表中移除
                         */
                        if (bitmap[n] == NGX_SLAB_BUSY) {
                            /*
                             * NOTE: 对于分割成了 < ngx_slab_exact_size 的内存块的 page，
                             *       一个 uintptr_t 是不够表示所有内存块的 bitmap 的，
                             *       所以需要多个(这里是 map 个) uintptr_t，
                             *       所以这里需要检查整个 bitmap 才能知道这个 page 是否为
                             *       满页
                             */
                            for (n = n + 1; n < map; n++) {
                                if (bitmap[n] != NGX_SLAB_BUSY) {
                                    goto done;
                                }
                            }

                            prev = ngx_slab_page_prev(page);
                            prev->next = page->next;
                            page->next->prev = page->prev;

                            /*
                             * NOTE: 把 prev 置为 NGX_SLAB_SMALL，也就是 3
                             *       后面使用 ngx_slab_page_prev 取前一个 page:
                                     ((page)->prev & ~NGX_SLAB_PAGE_MASK)
                             *       就会得到 NULL
                             */
                            page->next = NULL;
                            page->prev = NGX_SLAB_SMALL;
                        }

                        goto done;
                    }
                }
            }

        } else if (shift == ngx_slab_exact_shift) {

            for (m = 1, i = 0; m; m <<= 1, i++) {
                if (page->slab & m) {
                    continue;
                }

                page->slab |= m;

                if (page->slab == NGX_SLAB_BUSY) {
                    prev = ngx_slab_page_prev(page);
                    prev->next = page->next;
                    page->next->prev = page->prev;

                    page->next = NULL;
                    page->prev = NGX_SLAB_EXACT;
                }

                p = ngx_slab_page_addr(pool, page) + (i << shift);

                pool->stats[slot].used++;

                goto done;
            }

        } else { /* shift > ngx_slab_exact_shift */

            /*
             * NOTE: 大于 ngx_slab_max_size 的情况在最开始已经处理了，
             *       所以这里处理的是 (ngx_slab_exact_size, ngx_slab_max_size] 的情况
             *
             * NOTE: 对于 > ngx_slab_exact_shift 的内存块，其 slab 字段分为两部分
             *       高 32 bit 表示bitmap，低 32 bit 以位偏移的形式表示内存块大小
             */
            mask = ((uintptr_t) 1 << (ngx_pagesize >> shift)) - 1;
            mask <<= NGX_SLAB_MAP_SHIFT;

            for (m = (uintptr_t) 1 << NGX_SLAB_MAP_SHIFT, i = 0;
                 m & mask;
                 m <<= 1, i++)
            {
                if (page->slab & m) {
                    continue;
                }

                page->slab |= m;

                if ((page->slab & NGX_SLAB_MAP_MASK) == mask) {
                    prev = ngx_slab_page_prev(page);
                    prev->next = page->next;
                    page->next->prev = page->prev;

                    page->next = NULL;
                    page->prev = NGX_SLAB_BIG;
                }

                p = ngx_slab_page_addr(pool, page) + (i << shift);

                pool->stats[slot].used++;

                goto done;
            }
        }

        ngx_slab_error(pool, NGX_LOG_ALERT, "ngx_slab_alloc(): page is busy");
        ngx_debug_point();
    }

    /*
     * NOTE: 到这里说明在对应的 slot 里面没有找到可用的 page，所以需要从 free 链表里面
     *       再分配 1 页
     */
    page = ngx_slab_alloc_pages(pool, 1);

    if (page) {

        /*
         * NOTE: 新分配了一个 page，此时需要为它初始化 bitmap
         *       这里感觉有点晦涩
         */
        if (shift < ngx_slab_exact_shift) {
            bitmap = (uintptr_t *) ngx_slab_page_addr(pool, page);

            /*
             * NOTE: 1. ngx_pagesize >> shift 得到的是这个 page 可用放多少个内存块
             *          比如 shift = 5，也就是说内存块大小 32B，然后:
             *          ngx_pagesize >> shift = 128，也就是说可以放 128 个 32B 大小的内存块
             *       2. 1 << shift 为一个内存块的大小，按照上面的假设，就是 1 << 5 = 32B
             *          (1 << shift) * 8 也就是一个内存块的 bit 数，这里就是 32 * 8 = 256
             *       3. [1] / [2] 得到的就是这些内存块的 bitmap 需要用几个这样的内存块来放
             *          像这里，128 / 256 = 0.5，也就是说 1 个内存块就可以放了
             *          如果 shift = 4，那就需要 256 / 128 = 2 个内存块
             *          如果 shift = 3，那就需要 512 / 64 = 4 个内存块
             */
            n = (ngx_pagesize >> shift) / ((1 << shift) * 8);

            if (n == 0) {
                n = 1;
            }

            /* "n" elements for bitmap, plus one requested */

            /*
             * QUESTION: 这里为什么要把这些都设置为 NGX_SLAB_BUSY 呢？
             *           这里需要注意，bitmap 也是占用了实际内存的，而不像其他的情况一样
             *           用 ngx_slab_page_t::slab 字段来表示，所以 bitmap 自己本身所
             *           占有的内存是不能在分配出去的，所以需要在 bitmap 自己中体现出来，
             *           也就是把 bitmap 自己占用的内存给标记为 BUSY
             *
             * NOTE: (n + 1) / (8 * sizeof(uintptr_t)) 是看看这个 bitmap 需要多少
             *       个 uintptr_t，其中 +1 是加上了请求的内存块
             *
             * NOTE: 注意这里为什么要用一个 for 循环，按照 4K 的 page，min_shift = 3
             *       n+1 最大也就是 8。但是在有些 page 很大的系统上，比如 ppc64 的
             *       page 大小为 64KB，而如果 shift = 3，那么 n = 2^13 / 2^6 = 128，
             *       那么一个 uintptr_t 就放不下了
             *       所以这里修复了一次 bug，原来假设一个 uintptr_t 就可以放下：
             *       bitmap[0] = ((uintptr_t) 2 << n) - 1;
             *
             */
            for (i = 0; i < (n + 1) / (8 * sizeof(uintptr_t)); i++) {
                bitmap[i] = NGX_SLAB_BUSY;
            }

            /*
             * NOTE: 前面只是应对了大 pagesize 的情况，比如上面说的 pagesize = 64K，
             *       shift = 3 然后 n = 128，超出了一个 uintptr_t，对于一整个
             *       uintptr_t 都被占用的情况，就不用一个 bit 一个 bit 设置为 1 了，
             *       直接把整个 uintptr_t 设置为 BUSY 就好了，但是这样不足一个 uintptr_t
             *       的占用内存，比如这里外部请求的 1 个内存块就没有在上一个 for 循环中
             *       被标记，因为 n+1 = 129，无法整除 64，所以在 bitmap 中不能直接将一
             *       整个 uintptr_t 都设置为 BUSY，而需要一个一个 bit 来设置
             */
            m = ((uintptr_t) 1 << ((n + 1) % (8 * sizeof(uintptr_t)))) - 1;
            bitmap[i] = m;

            /*
             * NOTE: 把尚未使用的内存块在 bitmap 中标记为 0
             */
            map = (ngx_pagesize >> shift) / (8 * sizeof(uintptr_t));

            for (i = i + 1; i < map; i++) {
                bitmap[i] = 0;
            }

            /*
             * NOTE: 对于 < ngx_slab_exact_size 的内存块，其 ngx_slab_page_t 中的
             *       slab 字段以位偏移的方式存储内存块大小
             *
             * NOTE: 新的 page 放在链表头部，便于快速查找
             */
            page->slab = shift;
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_SMALL;

            slots[slot].next = page;

            /*
             * NOTE: total 记录的是这个 slot 总的内存块的个数
             */
            pool->stats[slot].total += (ngx_pagesize >> shift) - n;

            p = ngx_slab_page_addr(pool, page) + (n << shift);

            pool->stats[slot].used++;

            goto done;

        } else if (shift == ngx_slab_exact_shift) {

            page->slab = 1;
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_EXACT;

            slots[slot].next = page;

            pool->stats[slot].total += 8 * sizeof(uintptr_t);

            p = ngx_slab_page_addr(pool, page);

            pool->stats[slot].used++;

            goto done;

        } else { /* shift > ngx_slab_exact_shift */

            /*
             * NOTE: 对于 > ngx_slab_exact_shift 的内存块，其 slab 字段分为两部分
             *       高 32 bit 表示bitmap，低 32 bit 以位偏移的形式表示内存块大小
             */
            page->slab = ((uintptr_t) 1 << NGX_SLAB_MAP_SHIFT) | shift;
            page->next = &slots[slot];
            page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_BIG;

            slots[slot].next = page;

            pool->stats[slot].total += ngx_pagesize >> shift;

            p = ngx_slab_page_addr(pool, page);

            pool->stats[slot].used++;

            goto done;
        }
    }

    p = 0;

    pool->stats[slot].fails++;

done:

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, ngx_cycle->log, 0,
                   "slab alloc: %p", (void *) p);

    return (void *) p;
}


void *
ngx_slab_calloc(ngx_slab_pool_t *pool, size_t size)
{
    void  *p;

    ngx_shmtx_lock(&pool->mutex);

    p = ngx_slab_calloc_locked(pool, size);

    ngx_shmtx_unlock(&pool->mutex);

    return p;
}


void *
ngx_slab_calloc_locked(ngx_slab_pool_t *pool, size_t size)
{
    void  *p;

    p = ngx_slab_alloc_locked(pool, size);
    if (p) {
        ngx_memzero(p, size);
    }

    return p;
}


void
ngx_slab_free(ngx_slab_pool_t *pool, void *p)
{
    ngx_shmtx_lock(&pool->mutex);

    ngx_slab_free_locked(pool, p);

    ngx_shmtx_unlock(&pool->mutex);
}


void
ngx_slab_free_locked(ngx_slab_pool_t *pool, void *p)
{
    size_t            size;
    uintptr_t         slab, m, *bitmap;
    ngx_uint_t        i, n, type, slot, shift, map;
    ngx_slab_page_t  *slots, *page;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, ngx_cycle->log, 0, "slab free: %p", p);

    if ((u_char *) p < pool->start || (u_char *) p > pool->end) {
        ngx_slab_error(pool, NGX_LOG_ALERT, "ngx_slab_free(): outside of pool");
        goto fail;
    }

    n = ((u_char *) p - pool->start) >> ngx_pagesize_shift;
    page = &pool->pages[n];
    slab = page->slab;
    type = ngx_slab_page_type(page);

    switch (type) {

    case NGX_SLAB_SMALL:

        shift = slab & NGX_SLAB_SHIFT_MASK;
        size = (size_t) 1 << shift;

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        n = ((uintptr_t) p & (ngx_pagesize - 1)) >> shift;
        m = (uintptr_t) 1 << (n % (8 * sizeof(uintptr_t)));
        n /= 8 * sizeof(uintptr_t);
        bitmap = (uintptr_t *)
                             ((uintptr_t) p & ~((uintptr_t) ngx_pagesize - 1));

        if (bitmap[n] & m) {
            slot = shift - pool->min_shift;

            if (page->next == NULL) {
                slots = ngx_slab_slots(pool);

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_SMALL;
                page->next->prev = (uintptr_t) page | NGX_SLAB_SMALL;
            }

            bitmap[n] &= ~m;

            n = (ngx_pagesize >> shift) / ((1 << shift) * 8);

            if (n == 0) {
                n = 1;
            }

            i = n / (8 * sizeof(uintptr_t));
            m = ((uintptr_t) 1 << (n % (8 * sizeof(uintptr_t)))) - 1;

            if (bitmap[i] & ~m) {
                goto done;
            }

            map = (ngx_pagesize >> shift) / (8 * sizeof(uintptr_t));

            for (i = i + 1; i < map; i++) {
                if (bitmap[i]) {
                    goto done;
                }
            }

            ngx_slab_free_pages(pool, page, 1);

            pool->stats[slot].total -= (ngx_pagesize >> shift) - n;

            goto done;
        }

        goto chunk_already_free;

    case NGX_SLAB_EXACT:

        m = (uintptr_t) 1 <<
                (((uintptr_t) p & (ngx_pagesize - 1)) >> ngx_slab_exact_shift);
        size = ngx_slab_exact_size;

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        if (slab & m) {
            slot = ngx_slab_exact_shift - pool->min_shift;

            if (slab == NGX_SLAB_BUSY) {
                slots = ngx_slab_slots(pool);

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_EXACT;
                page->next->prev = (uintptr_t) page | NGX_SLAB_EXACT;
            }

            page->slab &= ~m;

            if (page->slab) {
                goto done;
            }

            ngx_slab_free_pages(pool, page, 1);

            pool->stats[slot].total -= 8 * sizeof(uintptr_t);

            goto done;
        }

        goto chunk_already_free;

    case NGX_SLAB_BIG:

        shift = slab & NGX_SLAB_SHIFT_MASK;
        size = (size_t) 1 << shift;

        if ((uintptr_t) p & (size - 1)) {
            goto wrong_chunk;
        }

        m = (uintptr_t) 1 << ((((uintptr_t) p & (ngx_pagesize - 1)) >> shift)
                              + NGX_SLAB_MAP_SHIFT);

        if (slab & m) {
            slot = shift - pool->min_shift;

            if (page->next == NULL) {
                slots = ngx_slab_slots(pool);

                page->next = slots[slot].next;
                slots[slot].next = page;

                page->prev = (uintptr_t) &slots[slot] | NGX_SLAB_BIG;
                page->next->prev = (uintptr_t) page | NGX_SLAB_BIG;
            }

            page->slab &= ~m;

            if (page->slab & NGX_SLAB_MAP_MASK) {
                goto done;
            }

            ngx_slab_free_pages(pool, page, 1);

            pool->stats[slot].total -= ngx_pagesize >> shift;

            goto done;
        }

        goto chunk_already_free;

    case NGX_SLAB_PAGE:

        if ((uintptr_t) p & (ngx_pagesize - 1)) {
            goto wrong_chunk;
        }

        if (!(slab & NGX_SLAB_PAGE_START)) {
            ngx_slab_error(pool, NGX_LOG_ALERT,
                           "ngx_slab_free(): page is already free");
            goto fail;
        }

        if (slab == NGX_SLAB_PAGE_BUSY) {
            ngx_slab_error(pool, NGX_LOG_ALERT,
                           "ngx_slab_free(): pointer to wrong page");
            goto fail;
        }

        size = slab & ~NGX_SLAB_PAGE_START;

        ngx_slab_free_pages(pool, page, size);

        ngx_slab_junk(p, size << ngx_pagesize_shift);

        return;
    }

    /* not reached */

    return;

done:

    pool->stats[slot].used--;

    ngx_slab_junk(p, size);

    return;

wrong_chunk:

    ngx_slab_error(pool, NGX_LOG_ALERT,
                   "ngx_slab_free(): pointer to wrong chunk");

    goto fail;

chunk_already_free:

    ngx_slab_error(pool, NGX_LOG_ALERT,
                   "ngx_slab_free(): chunk is already free");

fail:

    return;
}


static ngx_slab_page_t *
ngx_slab_alloc_pages(ngx_slab_pool_t *pool, ngx_uint_t pages)
{
    ngx_slab_page_t  *page, *p;

    for (page = pool->free.next; page != &pool->free; page = page->next) {

        if (page->slab >= pages) {

            /*
             * NOTE: free 链表中每个元素并不只是一个 page，可能多个连续的 page 只有第
             *       一个 page 充当链表节点，然后用 slab 字段表示该节点中 page 的个数
             *       所以如果这个 page 数组中 page 的个数大于想要的 page 个数，那么就
             *       得进行分割，否则的话直接将这个节点从 free 链表中移除即可
             */
            if (page->slab > pages) {
                page[page->slab - 1].prev = (uintptr_t) &page[pages];

                page[pages].slab = page->slab - pages;
                page[pages].next = page->next;
                page[pages].prev = page->prev;

                p = (ngx_slab_page_t *) page->prev;
                p->next = &page[pages];
                page->next->prev = (uintptr_t) &page[pages];

            } else {
                p = (ngx_slab_page_t *) page->prev;
                p->next = page->next;
                page->next->prev = page->prev;
            }

            /*
             * NOTE: 新分配的 page，还不知道它是用来存放多大内存块的
             */
            page->slab = pages | NGX_SLAB_PAGE_START;
            page->next = NULL;
            page->prev = NGX_SLAB_PAGE;

            /*
             * NOTE: 和 stat 字段一样，pfree 也是用来统计的
             */
            pool->pfree -= pages;

            if (--pages == 0) {
                return page;
            }

            /*
             * NOTE: 如果分配了多个页面，那么后续的页面也需要初始化
             *       连续页作为一个内存块一起分配出时，非第一页的 page 的 slab 都设置为 BUSY
             */
            for (p = page + 1; pages; pages--) {
                p->slab = NGX_SLAB_PAGE_BUSY;
                p->next = NULL;
                p->prev = NGX_SLAB_PAGE;
                p++;
            }

            return page;
        }
    }

    if (pool->log_nomem) {
        ngx_slab_error(pool, NGX_LOG_CRIT,
                       "ngx_slab_alloc() failed: no memory");
    }

    return NULL;
}


static void
ngx_slab_free_pages(ngx_slab_pool_t *pool, ngx_slab_page_t *page,
    ngx_uint_t pages)
{
    ngx_slab_page_t  *prev, *join;

    pool->pfree += pages;

    page->slab = pages--;

    if (pages) {
        ngx_memzero(&page[1], pages * sizeof(ngx_slab_page_t));
    }

    if (page->next) {
        prev = ngx_slab_page_prev(page);
        prev->next = page->next;
        page->next->prev = page->prev;
    }

    join = page + page->slab;

    if (join < pool->last) {

        if (ngx_slab_page_type(join) == NGX_SLAB_PAGE) {

            if (join->next != NULL) {
                pages += join->slab;
                page->slab += join->slab;

                prev = ngx_slab_page_prev(join);
                prev->next = join->next;
                join->next->prev = join->prev;

                join->slab = NGX_SLAB_PAGE_FREE;
                join->next = NULL;
                join->prev = NGX_SLAB_PAGE;
            }
        }
    }

    if (page > pool->pages) {
        join = page - 1;

        if (ngx_slab_page_type(join) == NGX_SLAB_PAGE) {

            if (join->slab == NGX_SLAB_PAGE_FREE) {
                join = ngx_slab_page_prev(join);
            }

            if (join->next != NULL) {
                pages += join->slab;
                join->slab += page->slab;

                prev = ngx_slab_page_prev(join);
                prev->next = join->next;
                join->next->prev = join->prev;

                page->slab = NGX_SLAB_PAGE_FREE;
                page->next = NULL;
                page->prev = NGX_SLAB_PAGE;

                page = join;
            }
        }
    }

    if (pages) {
        page[pages].prev = (uintptr_t) page;
    }

    page->prev = (uintptr_t) &pool->free;
    page->next = pool->free.next;

    page->next->prev = (uintptr_t) page;

    pool->free.next = page;
}


static void
ngx_slab_error(ngx_slab_pool_t *pool, ngx_uint_t level, char *text)
{
    ngx_log_error(level, ngx_cycle->log, 0, "%s%s", text, pool->log_ctx);
}
