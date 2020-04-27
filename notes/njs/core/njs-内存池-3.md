# njs 内存池（三）内存的分配和回收

## 内存的分配

```c
void *
njs_mp_alloc(njs_mp_t *mp, size_t size)
{
    if (size < mp->page_size) {
        return njs_mp_alloc_small(mp, size);
    }
    
    return njs_mp_alloc_large(mp, NJS_MAX_ALIGNMENT, size);
}
```

可以发现，如果要分配的内存大小小于一个页面大小(此处的页面和 OS 中页面的概念不同)，
就调用`njs_mp_alloc_small`进行内存的分配，否则调用`njs_mp_alloc_large`。 当然表
面上看是这样的，但是实际上有许多变化。

```c
static void *
njs_mp_alloc_small(njs_mp_t *mp, size_t size)
{
    u_char            *p;
    njs_mp_page_t     *page;
    njs_mp_slot_t     *slot;
    njs_queue_link_t  *link;
    
    if (size <= mp->page_size / 2) {
    
        for (slot = mp->slots; slot->size < size; slot++) { /* void */ }
        
        size = slot->size;

        if (njs_fast_path(!njs_queue_is_empty(&slot->pages))) {
            link = njs_queue_first(&slot->pages);
            page = njs_queue_link_data(link, njs_mp_page_t, link);

            p = njs_mp_page_addr(mp, page);
            p += njs_mp_alloc_chunk(page->map, size);

            page->chunks--;

            if (page->chunk == 0) {
                njs_queue_remove(page->link);
            }

        } else {
            page = njs_mp_alloc_page(mp);
            
            if (njs_fast_path(page != NULL)) {
                njs_queue_insert_head(&slot->pages, &page->link);
                
                page->map[0] = 0x80;
                page->map[1] = 0;
                page->map[2] = 0;
                page->map[3] = 0;
                
                page->chunks = slot->chunks;
                page->size = size >> mp->chunk_size_shift;
                
                p = njs_mp_page_addr(mp, page);
            }
        }

    } else {
    
    }
    
    return p;
}
```

如果需要分配的内存大小大于半页，那么由于`mp->slots`都是由小于一个页面大小的 chunk
组成，所以不能从里面来进行内存的分配。

需要分配的内存大小小于半页时的分配策略。这个时候需要从`mp->slots`中进行分配，首
先找到找到具有合适 chunk 大小的 slot。

* 如果该 slot 中还没有 page，那么首先得先分配一个 page，然后

注意`page->chunks = slot->chunks`这一句，的确我们已经在`page->map`中把第一个 chunk
标记为使用中(busy)了，而`page->chunks`表示的是该 page(由 chunk 组成)中剩下的空闲
chunk 的数量，这里本来应该是`page->chunks = slot->chunks - 1`才对的，但是其实在
`njs_mp_fast_create`中已经减了 1 了，所以这里就不用再减了，置于为什么要这样我暂
时还不清楚(TODO)。

* 如果该 slot 中已经有可用的 page 了，那么就从该 page 中进行内存分配。首先从 pages
队列中获取到可用的 page，然后使用`njs_mp_page_addr()`获取到该 page 的起始地址：

```c
njs_inline u_char *
njs_mp_page_addr(njs_mp_page_t _page)
{
    njs_mp_block_t  *block;
    
    block = (njs_mp_block_t *)
                ((u_char *) page - page->number * sizeof(njs_mp_page_t)
                 - offsetof(njs_mp_block_t, pages));
                 
    return block->start + (page->number << mp->page_size_shift);
}
```

这个函数有点难理解。但是可以确定的是它首先是要通过`page`来获取一个`njs_mp_block_t`
对象指针，怎么做呢？这个需要从`njs_mp_page_t`和`njs_mp_block_t`两个结构体的关系
来入手。

### 分配大块内存

```c
static void *
njs_mp_alloc_large(njs_mp_t *mp, size_t alignment, size_t size)
{
    u_char          *p;
    size_t          aligned_size;
    uint8_t         type;
    njs_mp_block_t  *block;
    
    if (njs_slow_path(size >= UINT32_MAX)) {
        return NULL;
    }
    
    if (njs_is_power_of_two(size)) {
        block = njs_malloc(sizeof(njs_mp_block_t));
        if (njs_slow_path(block == NULL)) {
            return NULL;
        }
        
        p = njs_memalign(alignment, size);
        if (njs_slow_path(p == NULL)) {
            njs_free(block);
            return NULL;
        }
        
        type = NJS_MP_DISCRETE_BLOCK;

    } else {
        aligned_size = njs_align_size(size, sizeof(uintptr_t));
        
        p = njs_memalign(alignment_size, aligned_size + sizeof(njs_mp_block_t))
        if (njs_slow_path(p == NULL)) {
            return NULL;
        }
        
        block = (njs_mp_block_t *) (p + aligned_size);
        type = NJS_MP_EMBEDDED_BLOCK;
    }
    
    block->type = type;
    block->size = size;
    block->start = p;
    
    njs_rbtree_insert(&mp->blocks, &block->node);
    
    return p;
}
```

分配大块内存时并不是从`mp->free_pages`链表上取，因为要分配的内存大于

* 首先确保需要分配的内存大小不超过 4G
* 然后根据所需要分配的内存大小是否为 2 的幂次，以此来决定所要分配的 cluster 的类型(TODO)
    - 如果是 2 的幂，那么它天然就是对，这种类型的 cluster 为`NJS_MP_DISCRETE_BLOCK`，因为
      实际的内存(即`block->start`)和 block 本身是分开来的
    - 如果不是 2 的幂，那么就使用`njs_align_size`来获取到对齐的大小
      这种 cluster 属于`NJS_MP_EMBEDED_BLOCK`，因为 block 本身和其持有的内存是同一块。

对于 cluster 的类型，有如下定义：

```c
typedef enum {
    /* Block of cluster.  The block is allocated apart of the cluster. */
    NJS_MP_CLUSTER_BLOCK = 0,
    /*
     * Block of large allocation.
     * The block is allocated apart of the allocation.
     */
    NJS_MP_DISCRETE_BLOCK,
    /*
     * Block of large allocation.
     * The block is allocated just after of the allocation.
     */
    NJS_MP_EMBEDDED_BLOCK,
} njs_mp_block_type_t;
```

当我们使用`njs_mp_alloc_cluster`创建`njs_mp_block_t`时，该类型的 cluster 默认就
是`NJS_MP_CLUSTER_BLOCK`，因为其值为 0，所以没有显式赋值。

在`njs_mp_alloc_large`函数中，当需要分配的内存大小不是 2 的倍数时，就会分配
`NJS_MP_EMBEDDED_BLOCK`类型的 cluster，此时`njs_mp_block_t`就在它所持有的内存的
最后面，所以才是 EMBEDDED。

    
有一个地方我觉得还是需要看一下的，就是`njs_align_size`函数

```c
#define njs_align_size(size, a)                                            \
    (((size) + ((size_t) a - 1)) & ~((size_t) a - 1))
```

仔细看一下可以发现这个函数其实是求的是最小的比`size`大的`a`的倍数。比如传入的
size 为 62，对齐量为 8，那么经过`njs_align_size`之后得到的是 64。

## 杂项

杂项是指尚未整理的部分。对于做笔记、写博客来说，我感觉条理是一个非常重要的东西。
当然旁征博引肯定也重要，但是对于现在的我来说还做不到，只能做好条理清晰这块了。

### cluster 的分配

我们已经知道`njs_mp_t`是由一些 cluster 组成的，而 cluster 是用`njs_mp_block_t`来
表示的，这些 cluster 被存储在`njs_mp_t::blocks`字段中，一开始内存池中肯定是没有
cluster 的，需要后面分配，那么首先来看看 cluster 是怎么分配的：

```c
static njs_mp_block_t *
njs_mp_alloc_cluster(njs_mp_t *mp)
{
    njs_uint_t      n;
    njs_mp_block_t  *cluster;
    
    n = mp->cluster_size >> mp->page_size_shift;
    
    cluster = njs_zalloc(sizeof(njs_mp_block_t) + n * sizeof(njs_mp_page_t));
    
    if (njs_slow_path(cluster == NULL)) {
        return NULL;
    }
    
    cluster->size = mp->cluster_size;
    
    cluster->start = njs_memalign(mp->page_alignment, cluster->size);
    if (njs_slow_path(cluster->start == NULL)) {
        njs_free(cluster);
        return NULL;
    }
    
    n--;
    cluster->pages[n].number = n;
    njs_queue_insert_head(&mp->free_pages, &cluster->pages[n].link);
    
    while (n != 0) {
        n--;
        cluster->pages[n].number = n;
        njs_queue_insert_before(&cluster->pages[n + 1].link,
                                &cluster->pages[n].link);
    }
    
    njs_rbtree_insert(&mp->blocks, &cluster->node);
    
    return cluster;
}
```

首先自然是要为`njs_mp_block_t`结构体本身分配内存了，由于它的`pages`字段是一个柔
性数组，所以得分配额外的内存。该分配多少个 page 呢？这个值是通过`cluster_size`和
`page_size_shift`这两字段算出来的(TODO)。

然后把该 cluster 中的一个 page 通过 `njs_mp_page_t::link` 字段挂到`njs_mp_t`的
`free_pages`链表上去以供使用，因为一般情况下调用`njs_mp_alloc_cluster`都是因为没
有 page 可供内存分配了。注意挂是挂在队列的头部，所以后面取用的时候也是从头部开始取用。

然后设置好该 cluster 中其余 page 的序号，并且将它们链接成一个链表，这里需要注意，
前面第一个 page 挂到了`free_pages`链表上去，然后这里剩余的 page 通过`while`循环
其实也挂到`free_pages`上去了。

最后把这个新创建的 cluster 挂载到`njs_mp_t`中的`blocks`红黑树上去以便管理。

### page 的分配

```c
static njs_mp_page_t *
njs_mp_alloc_page(njs_mp_t *mp)
{
    njs_mp_page_t     *page;
    njs_mp_block_t    *block;
    njs_queue_link_t  *link;
    
    if (njs_queue_is_empty(&mp->free_pages)) {
        cluster = njs_mp_alloc_cluster(mp);
        if (njs_slow_path(cluster == NULL)) {
            return NULL;
        }
    }
    
    link = njs_queue_first(&mp->free_pages);
    njs_queue_remove(link);
    
    page = njs_queue_link_data(link, njs_mp_page_t, link);
    
    return page;
}
```

可以发现首先是检查`free_pages`中是否还有空闲的 page 可用，如果没有的 haul，就得
分配 cluster 了，分配了 cluster 之后`free_pages`链表上就有新的空闲 page 了，取下
第一个 page 即可。

### chunk 的分配

```c
static njs_uint_t
njs_mp_alloc_chunk(uint8_t *map, njs_uint_t size)
{
    uint8_t     mask;
    njs_uint_t  n, offset;
    
    offset = 0;
    n = 0;
    
    /* The page must have at least one free chunk. */
    
    for ( ;; ) {
        if (map[n] != 0xff) {
            mask = 0x80;

            do {
                if (map[n] & mask == 0) {
                    /* A free chunk is found. */
                    map[n] |= mask;
                    return offset;
                }
                
                offset += size;
                mask >>= 1;

            } while (mask != 0);

        } else {
            /* Fast-forward: all 8 chunks are occupied. */
            offset += size * 8;
        }
    }
}
```

前面已经说过了，每个 page 最多不包括 32 个 chunk，这些 chunk 的使用情况都被包含
在`map`位图中。再看函数的返回值，返回的是一个无符号数，也就是该 chunk 在所属 page
中的偏移量。

所以首先就是在位图中寻找第一个空闲的 chunk，对应 bit 上的值为 1 表示该 chunk 已
被使用(busy)。这个查找的方式还是很值得思考的，首先我们知道这个位图是由 4 个`uint8_t`
组成，所以为它准备了一个快车道(Fast-forward)，如果某个`uint8_t`表示的 8 个 chunk
都被使用了，就直接跳过就可以了。否则在这 8 个 chunk 中一个一个找，找到了还要记得
把它所对应的 bit 置为 1。由于传入的`size`参数是该 page 中 chunk 的大小(某个 page
中所有 chunk 都具有相同大小)，所以可以很容易地找到第一个空闲的 chunk 的偏移量。

TODO: 但是怎么保证一定找得到呢？会不会这个 page 里面没有一个 chunk 是空闲的呢？看注释
说是不会，为什么呢？怎么保证的呢？

## 内存的回收

```c
void
njs_mp_free(njs_mp_t *mp, void *p)
{
    const char      *err;
    njs_mp_block_t  *block;
    
    block = njs_mp_find_block(&mp->blocks, p);
    
    if (njs_fast_path(block != NULL)) {
    
        if (block->type == NJS_MP_CLUSTER_BLOCK) {
            err = njs_mp_chunk_free(mp, block, p);
            
            if (njs_fast_path(err == NULL)) {
                return;
            }
        
        } else if (njs_fast_path(p == block->start)) {
            njs_rbtree_delete(&mp->blocks, block->node);
            
            if (block->type == NJS_MP_DISCRETE_BLOCK) {
                njs_free(block);
            }
            
            njs_free(p);

            return;
        
        } else {
            err = "freed pointer points middle of block: %p\n";
        }

    } else {
        err = "freed pointer is out of mp: %p\n";
    }
    
    njs_debug_alloc(err, p);
}
```

既然要回收内存，而所有的内存都是以 cluster 的形式(也就是`njs_mp_block_t`，无论是
small 还是 large)进行分配的，而所有的 cluster 都被放在`mp->blocks`红黑树中，所以
首先得找到所释放的内存所在的 cluster。

找到了 cluster 之后，根据该 cluster 的类型来决定具体该如何回收内存。如果 cluster
为`NJS_MP_CLUSTER_BLOCK`，那么说明这块内存是通过`njs_mp_alloc_small`分配而来，那
么就是

如果 cluster 的类型不为`NJS_MP_CLUSTER_BLOCK`，那么这个 cluster 就是通过`njs_mp_alloc_large`
分配而来。

```c
static const char *
njs_mp_chunk_free(njs_mp_t *mp, njs_mp_block_t *cluster,
    u_char *p)
{

}
```

## 总结

 经过上面的源码剖析我们知道了 njs 内存池有一些 cluster 组成，在源码中表现为
 `njs_mp_block_t`结构；每个 cluster 由许多 page 构成，在源码中表现为`njs_mp_page_t`
 结构；每个 page 被切分为许多个 cluster，对于某个 page 而言，组成它的 chunk 的大
 小都是相同的，但是 cluster 中的 page 虽然大小相同，但是构成它们的 chunk 却是不
 同规格的。
 
既然 cluster 有`njs_mp_block_t`表示，并且所有的 cluster 以红黑树的形式放在了
`njs_mp_t::blocks`字段中；page 则有`njs_mp_page_t`，并且某个 cluster 中的所有 page
都被放在了`njs_mp_block_t::pages`这个柔性数组中了。按照这个规律 page 中的所有 chunk
也应该放在`njs_mp_page_t`中的某个字段中吧？但是实际上所有的 chunk 都被放入了
`njs_mp_t`结构的`slots`柔性数组中了。

为什么要把 chunk 直接放到`njs_mp_t`中而不是`njs_mp_page_t`中呢？我觉得有两点：

1. 方便取用。因为 chunk 是内存分配的直接单位，所以在进行内存分配时如果先取出
cluster，然后再 page，最后再 chunk，这样就太繁琐了，所以一步到位更直接一点。
2. 我们已经知道了一个 cluster 中的所有 page 虽然大小相同，但是组成这些 page 的
chunk 的规格却是不同，而一个`njs_mp_t`内存池中不仅仅只有一个 cluster，多个 cluster
的话就会有具有相同大小的、并且 chunk 规模相同的 page，而`njs_mp_slot_t`中有一个
`pages`链表节点，就是用来链接不同 cluster 中所有具有该 chunk 规格(用`njs_mp_slot_t`
来表示)的 page。这样一来，chunk 就不单单只属于某个 page 了，它还链接这其他 cluster
中具有该 chunk 规格的 page，也就是说 chunk 和 page 就不只是简单的从属关系了。

还有一个地方值得注意。
事实上只有 cluster 才持有内存，创建 cluster 时就使用`njs_memalign`分配了这个
cluster 所持有的所有内存(`cluster_size`指定)，而不是说每个 page 都持有一部分内存，
或者说每个 chunk 都持有一部分内存。page 和 chunk 只是两个管理单位，最终的内存还是
得从 `cluster->start`里面拿。但是由于 cluster 持有的是一整块大内存，而且这块内存
已经被分割成了许多 page，然后每个 page 又被分割成了许多 chunk，所以分配内存的时候
得找到可用的、合适的内存地址，而这一步其实是最繁琐的。
