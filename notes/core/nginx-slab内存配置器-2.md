# nginx slab allocator（二）

## nginx slab 提供的接口

slab allocator 只提供了很少的几个接口：

```c
void ngx_slab_sizes_init(void);
void ngx_slab_init(ngx_slab_pool_t *pool);
void *ngx_slab_alloc(ngx_slab_pool_t *pool, size_t size);
void *ngx_slab_alloc_locked(ngx_slab_pool_t *pool, size_t size);
void *ngx_slab_calloc(ngx_slab_pool_t *pool, size_t size);
void *ngx_slab_calloc_locked(ngx_slab_pool_t *pool, size_t size);
void ngx_slab_free(ngx_slab_pool_t *pool, void *p);
void ngx_slab_free_locked(ngx_slab_pool_t *pool, void *p);
```

非常的精简，但是奇怪的是居然没有`ngx_slab_create_pool`这样的创建接口，那么用什么来创建一个 slab allocator 呢？

### slab 和 zone 的配合使用

前面说过，slab 通常是和 zone 一起用的：

```c
typedef struct ngx_shm_zone_s  ngx_shm_zone_t;

typedef ngx_int_t (*ngx_shm_zone_init_pt) (ngx_shm_zone_t *zone, void *data);

struct ngx_shm_zone_s {
    void                     *data;
    ngx_shm_t                 shm;
    ngx_shm_zone_init_pt      init;
    void                     *tag;
    void                     *sync;
    ngx_uint_t                noreuse;  /* unsigned  noreuse:1; */
};

typedef struct {
    u_char      *addr;
    size_t       size;
    ngx_str_t    name;
    ngx_log_t   *log;
    ngx_uint_t   exists;   /* unsigned  exists:1;  */
} ngx_shm_t;
```

TODO: zone 在 nginx 中是一个什么概念？

其中`addr`字段就指向`ngx_slab_pool_t`，那么 zone 是怎么创建的呢？举个例子，limit_conn 模块中有个指令`limit_conn_zone`用于声明一个 zone，比如`limit_conn_zone name=addr:10m`表示创建一个名为`addr`大小为 10m 的 zone，在解析这个配置项时，就会通过`ngx_shared_memory_add`添加这样一个共享内存（但是不会实际创建它，后续解析完整个配置文件之后会统一创建）

```c
ngx_shm_zone_t *
ngx_shared_memory_add(ngx_conf_t *cf, ngx_str_t *name, size_t size, void *tag)
{
    ngx_uint_t        i;
    ngx_shm_zone_t   *shm_zone;
    ngx_list_part_t  *part;

    part = &cf->cycle->shared_memory.part;
    shm_zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            shm_zone = part->elts;
            i = 0;
        }

        if (name->len != shm_zone[i].shm.name.len) {
            continue;
        }

        if (ngx_strncmp(name->data, shm_zone[i].shm.name.data, name->len)
            != 0)
        {
            continue;
        }

        if (tag != shm_zone[i].tag) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "the shared memory zone \"%V\" is "
                            "already declared for a different use",
                            &shm_zone[i].shm.name);
            return NULL;
        }

        if (shm_zone[i].shm.size == 0) {
            shm_zone[i].shm.size = size;
        }

        if (size && size != shm_zone[i].shm.size) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "the size %uz of shared memory zone \"%V\" "
                            "conflicts with already declared size %uz",
                            size, &shm_zone[i].shm.name, shm_zone[i].shm.size);
            return NULL;
        }

        return &shm_zone[i];
    }

    shm_zone = ngx_list_push(&cf-c题，所以需要尽量重用已有的共享内存。

old_cycle 的逻辑暂且不细究，根据上面的代码可知创建新的 zone 需要调用 3 个函数。首先是`ngx_shm_alloc`函数：

```c
ngx_int_t
ngx_shm_alloc(ngx_shm_t *shm)
{
    shm->addr = (u_char *) mmap(NULL, shm->size,
                                PROT_READ|PROT_WRITE,
                                MAP_ANON|MAP_SHARED, -1, 0);

    if (shm->addr == MAP_FAILED) {
        ngx_log_error(NGX_LOG_ALERT, shm->log, ngx_errno,
                      "mmap(MAP_ANON|MAP_SHARED, %uz) failed", shm->size);
        return NGX_ERROR;
    }

    return NGX_OK;
}
```

没什么好说的，就是调用`mmap`这个系统调用创建共享内存，需要注意的是共享内存的地址由`ngx_shm_t::addr`这个字段指向，前面说过，这个地址其实也就是`ngx_slab_pool_t`的首地址，这个可以在后续的函数中看到。

然后是`ngx_init_zone_pool`函数：

```c


static ngx_int_t
ngx_init_zone_pool(ngx_cycle_t *cycle, ngx_shm_zone_t *zn)
{
    ngx_slab_pool_t  *sp;

    sp = (ngx_slab_pool_t *) zn->shm.addr;

    if (zn->shm.exists) {

        if (sp == sp->addr) {
            return NGX_OK;
        }

        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                      "shared zone \"%V\" has no equal addresses: %p vs %p",
                      &zn->shm.name, sp->addr, sp);
        return NGX_ERROR;
    }

    sp->end = zn->shm.addr + zn->shm.size;
    sp->min_shift = 3;
    sp->addr = zn->shm.addr;

    if (ngx_shmtx_create(&sp->mutex, &sp->lock, NULL) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_slab_init(sp);

    return NGX_OK;
}
```

函数也很简单，做了一些初步的初始化工作。前面说过，slab 中内存块的最小值为 8B，这个限制就是在这里设置的。然后就是调用了`ngx_slab_init`函数，这个函数比较长，后面再看。最后还调用了`ngx_shm_zone_t::init`这个回调函数，这个函数是使用到了 slab 的模块所必须设置的，比如`ngx_http_limit_conn`模块就设置了：

```c
static ngx_int_t
ngx_http_limit_conn_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_conn_ctx_t  *octx = data;

    size_t                      len;
    ngx_http_limit_conn_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
      ...
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
  
  	...

    len = sizeof(" in limit_conn_zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_conn_zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}

```

其中大部分逻辑是和该模块本身相关的，但是和 slab 相关的也有几点。比如前面已经看到`ngx_slab_pool_t`和`ngx_shm_zone_t`都有一个`addr`字段，而且说都是指向`ngx_slab_pool_t`。这里暂且没有看到`ngx_slab_pool_t`中的`adrr`字段，但是可以看到通过`ngx_shm_zone_t`的`addr`字段获取到了`ngx_slab_pool_t`。

然后就是前面说过的`ngx_slab_pool_t::log_ctx`字段了，这个字段默认指向的是零字符，但是为了调试方便，所以一般会自行设置，比如说这里就设置了。而且需要注意的是，`log_ctx`指向的内存也是从 slab 中分配的。

## 初始化 slab

slab 的初始化工作主要是在`ngx_slab_init`这个函数中做的，这个函数主要做 page 相关的构建工作，在此之前先来看看 page 的描述结构体：

### `ngx_slab_page_t`结构解析

每个 page 都有一个`ngx_slab_page_t`结构体用于描述该 page，这是一个多功能的结构体：

```c
typedef struct ngx_slab_page_s  ngx_slab_page_t;

struct ngx_slab_page_s {
    uintptr_t         slab;
    ngx_slab_page_t  *next;
    uintptr_t         prev;
};
```

前面说过，所有的 page 被分为**空闲页**、**半满页**和**全满页**这三种，全满页不在任何链表中；所有的空闲页组成一个双向循环链表，`ngx_slab_pool_t::free`字段就作为这个链表的哑头节点。对于`free`链表，有一点需要注意，并不是每一个空闲页都单独作为链表中的一个节点，可能存在连续多个页面只有首页面在 free 链表中占有一个节点的情况，此时`ngx_slab_page_t::slab`表示这个节点中空闲页的个数。

比如下面一共有 6 个 page，其中第 4 个是一个全满页，其他都是空闲页。前三个共用一个 free 链表节点，此时第一个 page 的`slab`字段值为 3，表示这个节点有 3 个 page，第五、六个 page 虽然相邻，但是分配、释放的时机不一致，导致这两个相邻不相识，所以每个都独自占有一个 free 链表节点：

![nginx-slab-free-linkedlist](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/slab/nginx-slab-free-linklist.jpeg)

### slab 结构的构建

前面说过用于 slab 的整块共享内存实际上是被分为 7 个部分的，这里就是来构建这个数据结构：

```c
void
ngx_slab_init(ngx_slab_pool_t *pool)
{
    u_char           *p;
    size_t            size;
    ngx_int_t         m;
    ngx_uint_t        i, n, pages;
    ngx_slab_page_t  *slots, *page;

    pool->min_size = (size_t) 1 << pool->min_shift;

    slots = ngx_slab_slots(pool);

    p = (u_char *) slots;
    size = pool->end - p;

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
        slots[i].next = &slots[i];
        slots[i].prev = 0;
    }

    p += n * sizeof(ngx_slab_page_t);

    pool->stats = (ngx_slab_stat_t *) p;
    ngx_memzero(pool->stats, n * sizeof(ngx_slab_stat_t));

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

    /* only "next" is used in list head */
    pool->free.slab = 0;
    pool->free.next = page;
    pool->free.prev = 0;

    page->slab = pages;
    page->next = &pool->free;
    page->prev = (uintptr_t) &pool->free;

    pool->start = ngx_align_ptr(p + pages * sizeof(ngx_slab_page_t),
                                ngx_pagesize);

    m = pages - (pool->end - pool->start) / ngx_pagesize;
    if (m > 0) {
        pages -= m;
        page->slab = pages;
    }

    pool->last = pool->pages + pages;
    pool->pfree = pages;

    pool->log_nomem = 1;
    pool->log_ctx = &pool->zero;
    pool->zero = '\0';
}
```

逻辑也比较简单，主要是这几个步骤：

1. 为 slots 数组和 stats 数组留下内存，并且初始化 slots 数组中各个节点的`next`指针指向自己
2. 计算 pages 数组的大小，设置第一个`ngx_slab_page_t`的`slab`字段值为数组大小，同时链接至`free`链表
3. 将真正所有页面所在内存的地址以页面大小进行对齐，这时可能有 padding 导致可用 page 数减少，需要更新 pages 数组中第一个元素的`slab`字段值
4. 设置`ngx_slab_pool_t::zero`为零字符，并且将`log_ctx`字段指向它

## 获取内存

slab 内存分配主要涉及到从半满页中分配内存块和从空闲页上分配新页面。从半满页上分配内存涉及到从 bitmap 中查找可用内存，但是 bitmap 所在位置根据页面划分的内存块的大小而有所不同。

从前面知道，`ngx_slab_page_t::slab`字段在空闲页中表示的是该链表节点实际上有多少个连续的页面，但是在半满页中没有这种意义了，所以可以用来充当 bitmap。但是这个字段长度为 64bit（假设是在 x64）上，可以追踪的内存块的个数有限，再多就用`slab`字段就存不下了。所以 nginx 依据`ngx_slab_page_t::slab`字段能否保存下该 page 中的所有内存块对应的 bitmap，而将 page 分为了 4 种：

```c
#define NGX_SLAB_PAGE_MASK   3
#define NGX_SLAB_PAGE        0
#define NGX_SLAB_BIG         1
#define NGX_SLAB_EXACT       2
#define NGX_SLAB_SMALL       3
```

首先是`EXACT`，顾名思义就是说`ngx_slab_page_t::slab`字段恰好可以放下这个 page 中的所有内存块对应的 bitmap，比如在 4KB 的页面中，64B 就是“恰好”的大小。而相应的，`SMALL`就是比`EXACT`要小的，而`BIG`则是比`EXACT`要大，但是比小于等于 pagesize/2 ，`PAGE`自然就是大于 pgesize/2 的。

这里`BIG`和`PAGE`这两种类型有点难理解，为什么需要有 pagesize/2 这个概念呢？TODO

当内存块的大小超过了`EXACT`的时候，`ngx_slab_page_t::slab`字段对于 bitmap 来说已经足够而且有多余了，最多用 16bit 就足够存放该 page 的 bitmap 了；对于内存块小于`EXACT`的 page，其`slab`字段不能用来存储 bitmap，bitmap 也多了出来，但是还有一个问题是，我们拿到一个 page，怎么知道这个 page 上存储的内存块的大小呢？（当然我们遇到一个内存分配请求时，是可以知道要从哪个 slot 中分配内存）所以这个数据也得存下来，这里就`slab`字段就派上用场了。

```c
static ngx_uint_t  ngx_slab_max_size;
static ngx_uint_t  ngx_slab_exact_size;
static ngx_uint_t  ngx_slab_exact_shift;
```



| page 类型        | `slab`字段含义 | `prev`字段含义 |
| ---------------- | -------------- | -------------- |
| `NGX_SLAB_SMALL` |                |                |
| `NGX_SLAB_EXACT` |                |                |
| `NGX_SLAB_BIG`   |                |                |
| `NGX_SLAB_PAGE`  |                |                |

这就是这 4 种

## 回收内存

ycle->shared_memory);

    if (shm_zone == NULL) {
        return NULL;
    }

    shm_zone->data = NULL;
    shm_zone->shm.log = cf->cycle->log;
    shm_zone->shm.addr = NULL;
    shm_zone->shm.size = size;
    shm_zone->shm.name = *name;
    shm_zone->shm.exists = 0;
    shm_zone->init = NULL;
    shm_zone->tag = tag;
    shm_zone->noreuse = 0;

    return shm_zone;
}

```

代码很简单，首先就是在`ngx_cycle_t::shared_memory`链表中查找是否已经有了相同的 zone，没有的话就往里面添加一个`ngx_shm_zone_t`结构体，此时只设置了`log`和传入的`name`、`tag`和`name`等字段，并没有实际创建共享内存。

在解析完整个配置文件后，开始遍历`ngx_cycle_t::shared_momory`链表，依次创建共享内存，这个是在`ngx_init_cycle`初始化函数中做的：

```c
ngx_cycle_t *
ngx_init_cycle(ngx_cycle_t *old_cycle)
{
    ...
    /* 解析配置文件 */
    ...
    
    /* create shared memory */
      
    part = &cycle->shared_memory.part;
    shm_zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            shm_zone = part->elts;
            i = 0;
        }
      
      	// 处理和 old_cycle 相关的逻辑
      	...
      
        if (ngx_shm_alloc(&shm_zone[i].shm) != NGX_OK) {
            goto failed;
        }

        if (ngx_init_zone_pool(cycle, &shm_zone[i]) != NGX_OK) {
            goto failed;
        }

        if (shm_zone[i].init(&shm_zone[i], NULL) != NGX_OK) {
            goto failed;
        }
    }
  
  	...   
}
```

这部分逻辑比较长，主要是需要处理 old_cycle 中的共享内存，比如我们`nginx -s reload`更新配置文件时，此前的正在使用的共享内存可能是还有数据的，如果我们直接丢弃不用而直接开辟新的内存，很可能造成严重问>
