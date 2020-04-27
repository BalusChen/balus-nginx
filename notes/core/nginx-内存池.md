
# Nginx内存池
## 主要结构

```c
typedef struct ngx_pool_s ngx_poll_t;

struct ngx_pool_s {
    ngx_pool_data_t       d;
    size_t                max;
    ngx_pool_t           *current;
    ngx_chain_t          *chain;
    ngx_pool_large_t     *large;
    ngx_pool_cleanup_t   *cleanup;
    ngx_log_t            *log;
};

typedef struct {
    u_char          *last;
    u_char          *end;
    ngx_pool_t      *next;
    ngx_uint_t       failed;
} ngx_pool_data_t;
```

讲一下这个结构体中各个域的作用：

* `max`:
* `current`: 当前使用的内存池，由于内存池是一个链式结构(`d`字段结构中有一个`next`字段)
* `chain`:
* `large`:
* `log`: 对该内存池的操作都会记录在此log文件中

其中`d`字段是最主要的内存块，用以分配内存:

* `last`: 下一次内存分配开始的地址
* `end`: 该内存池的内存末尾地址
* `next`:
* `failed`: 该内存池分配内存失败的次数

```c
typedef void (*ngx_pool_cleanup_pt)(void *data);

struct ngx_pool_cleanup_s {
    ngx_pool_cleanup_pt   handler;
    void                 *data;
    ngx_pool_cleanup_t   *next;
};
```

## 主要操作

### 创建内存池

```c
ngx_pool_t *
ngx_create_pool(size_t size, ngx_log_t *log)
{
    ngx_pool_t  *p;

    p = ngx_memalign(NGX_POOL_ALIGNMENT, size, log);
    if (p == NULL) {
        return NULL;
    }

    // NOTE: 从这里可以看出 d 字段结构的
    p->d.last = (u_char *) p + sizeof(ngx_pool_t);
    p->d.end = (u_char *) p + size;
    p->d.next = NULL;
    p->d.failed = 0;

    size = size - sizeof(ngx_pool_t);
    // 允许从内存池中分配的最大内存量
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL;

    p->current = p;
    p->chain = NULL;
    p->large = NULL;
    p->cleanup = NULL;
    p->log = log;

    return p;
}
```

有几点需要注意:

* 可以看出，实际分配内存的操作是`ngx_memalign`:

```c
void *
ngx_memalign(size_t aligment, size_t size, ngx_log_t *log)
{
    void  *p;
    int    err;

    err = posix_memalign(&p, alignment, size);

    if (err) {
        ngx_log_error(NGX_LOG_EMERG, log, err,
                      "posix_memalign(%uz, %uz) failed", alignment, size);
        p = NULL;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_ALLOC, log, 0
                   "posix_memalign: %p:%uz, @%uz", p, size, alignment);

    return p;
}
```

其实最主要的依据就是`posix_memalign(&p, alignment, size)`，这是一个系统函数，用以分配对齐的内存，可以通过`man 3 posix_memalign`来查看其用法。

* `max`字段规定了一次内存分配最多可以多少字节。是传给`ngx_create_pool`的`size`参数和`NGX_MAX_ALLOC_FROM_POOL`中的较小者。

```c
#defien NGX_MAX_ALLOC_FROM_POOL  (ngx_pagesize - 1)
```

在x86中一页的大小是4k，说明最多可以分配4095个字节。

### 从内存池中获取内存

```c
// 分配对齐的内存
void *
ngx_palloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) {
        return ngx_palloc_small(pool, size, 1);
    }
#endif

    return ngx_palloc_large(pool, size);
}

// 分配无需对齐的内存
void *
ngx_pnalloc(ngx_pool_t *pool, size_t size)
{
#if !(NGX_DEBUG_PALLOC)
    if (size <= pool->max) {
        return ngx_palloc_small(pool, size, 0);
    }
#endif

    return ngx_palloc_large(pool, size);
}
```

一个函数分配对齐的内存，另一个则用来分配无需对齐的内存。对齐与否是根据`ngx_palloc_small`和`ngx_palloc_large`的第三个参数来决定的。根据请求分配的内存大小采用两种不同的分配方式，小块内存使用的是`ngx_palloc_small`:

#### 分配小块内存

```c
static ngx_inline void *
ngx_palloc_small(ngx_pool_t *pool, size_t size, ngx_uint_t align)
{
    u_char      *m;
    ngx_pool_t  *p;

    p = pool->current;

    do {
        m = p->d.last;

        if (align) {
            // 获取要对齐的内存首地址
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
        }

        // 检查该内存池中的剩余的内存是否足够
        if ((size_t) (p->d.end - m) >= size) {
            p->d.last = m + size;

            return m;
        }

        // 不够则转到下一个内存池
        p = p->d.next;

    } while (p);

    // 整个内存池链表都没有足够内存了
    return ngx_palloc_block(pool, size);
}
```

可以看出分配流程：

1. 首先从当前内存池`current`开始查找，在内存池链表中查找足够大小的内存地址来进行分配。
2. 遍历完整个内存池链表都没有找到足够大小的内存空间以供分配，就使用`ngx_palloc_block`来进行分配:

```c

```

```c
static void *
ngx_palloc_block(ngx_pool_t *pool, size_t size)
{
    u_char      *m;
    size_t       psize;
    ngx_pool_t  *p, *new;

    // 当前pool的总大小
    psize = (size_t) (pool->d.end - (u_char *) pool);

    // 分配和pool一样大的内存
    m = ngx_memalign(NGX_POOL_ALIGNMENT, psize, pool->log);
    if (m == NULL) {
        return NULL;
    }

    new = (ngx_pool_t *) m;

    // 只设置ngx_pool_t中的d域(ngx_pool_data_t)
    new->d.end = m + psize;
    new->d.next = NULL;
    new->d.failed = 0;

    // m此时指向可用的空闲内存域
    m += sizeof(ngx_pool_data_t);
    m = ngx_align_ptr(m, NGX_ALIGNMENT);
    // 分配新的内存(size大小)
    new->d.last = m + size;

    for (p = pool->current; p->d.next; p = p->d.next) {
        if (p->d.failed++ > 4) {    // TODO: d.failed++ > 4???
            pool->current = p->d.next;
        }
    }

    p->d.next = new;

    return m;
}
```

#### 分配大块内存

```c
static void *
ngx_palloc_large(ngx_pool_t *pool, size_t size)
{
    void              *p;
    ngx_uint_t         n;
    ngx_pool_large_t  *large;

    // ngx_alloc实际上就是malloc，外加日志记录
    p = ngx_alloc(size, pool->log);
    if (p == NULL) {
        return NULL;
    }

    n = 0;

    // 挂在pool->large链表上
    for (large = pool->large; large; large = large->next) {
        // 向后搜寻空位用以挂载链表
        if (large->alloc == NULL) {
            large->alloc = p;
            return p;
        }

        // 如果 large 链表的长度 > 3，则挂载在链表头部
        if (n++ > 3) {
            break;
        }
    }

    // pool->large域还没有分配内存(ngx_pool_t初始化时其large域为NULL)
    // 从内存池中为ngx_pool_large_t获取内存
    // 而不是继续以malloc为ngx_pool_large_t分配内存
    large = ngx_palloc_small(pool, sizeof(ngx_pool_large_t), 1);
    if (large == NULL) {
        ngx_free(p);
        return NULL;
    }

    // 链接到pool->large链表的头部
    large->alloc = p;
    large->next = pool->large;
    pool->large = large;

    return p;
}
```

### 获取对齐的地址

```c
#define ngx_align_ptr(p, a)                                                   \
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))
```

### 内存池的销毁

```c
void
ngx_destroy_pool(ngx_pool_t *pool)
{
    ngx_pool_t          *p, *n;
    ngx_pool_large_t    *l;
    ngx_pool_cleanup_t  *c;

    // 释放一些特殊资源
    for (c = pool->cleanup; c; c = c->next) {
        if (c->handler) {
            ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                           "run cleanup: %p", c);
            c->handler(c->data);
        }
    }

#if (NGX_DEBUG)

    /*
     * we could allocate the pool->log from this pool
     * so we cannot use this log while free()ing the pool
     */

    for (l = pool->large; l; l = l->next) {
        ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, pool->log, 0, "free: %p", l->alloc);
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_log_debug2(NGX_LOG_DEBUG_ALLOC, pool->log, 0,
                       "free: %p, unused: %uz", p, p->d.end - p->d.last);

        if (n == NULL) {
            break;
        }
    }

#endif

    // 释放大块内存
    for (l = pool->large; l; l = l->next) {
        if (l->alloc) {
            ngx_free(l->alloc);
        }
    }

    for (p = pool, n = pool->d.next; /* void */; p = n, n = n->d.next) {
        ngx_free(p);

        if (n == NULL) {
            break;
        }
    }
}
```

这里有一个需要的地方就是要在销毁内存之前打日志，而不能一边打一边销毁，因为有可能`pool->log`的内存也是从`pool`分配而来。

### 有关特殊资源

```c
ngx_pool_cleanup_t *
ngx_pool_cleanup_add(ngx_pool_t *p, size_t size)
{
    ngx_pool_cleanup_t  *c;

    c = ngx_palloc(p, sizeof(ngx_pool_cleanup_t));
    if (c == NULL) {
        return NULL;
    }

    if (size) {
        c->data = ngx_palloc(p, size);
        if (c->data == NULL) {
            return NULL;
        }

    } else {
        c->data = NULL;
    }

    c->handler = NULL;
    c->next = p->cleanup;

    p->cleanup = c;

    ngx_log_debug1(NGX_LOG_DEBUG_ALLOC, p->log, 0, "add cleanup: %p", c);

    return c;
}
```

```c
void ngx_reset_pool(ngx_pool_t *pool);

ngx_int_t ngx_pfree(ngx_pool_t *pool, void *p);

ngx_pool_cleanup_t *ngx_pool_cleanup_add(ngx_pool_t *p, size_t size);

void ngx_pool_run_cleanup_file(ngx_pool_t *p, ngx_fd_t fd);

void ngx_pool_cleanup_file(void *data);

void ngx_pool_delete_file(void *data);
```

## 总结
