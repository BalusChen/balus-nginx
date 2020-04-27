# njs 内存池（一）

和 nginx base 一样，njs 也提供了内存池这种数据结构。但是和 nginx base 里面的`ngx_pool_t`却有很大的不同，反倒是和 nginx 中的`ngx_slab_pool_t`很相似。

首先代码注释中对这个内存池有一个介绍：

> A memory cache pool allocates memory in clusters of specified size and
> aligned to page_alignment.  A cluster is divided on pages of specified
> size.  Page size must be a power of 2.  A page can be used entirely or
> can be divided on chunks of equal size.  Chunk size must be a power of 2.
> A cluster can contains pages with different chunk sizes.  Cluster size
> must be a multiple of page size and may be not a power of 2.  Allocations
> greater than page are allocated outside clusters.  Start addresses and
> sizes of the clusters and large allocations are stored in rbtree blocks
> to find them on free operations.  The rbtree nodes are sorted by start
> addresses.

首先有个大致的印象：

* `njs_mp_t`从 OS 中分配内存是以**cluster**为单位
* 每个`cluster`都被分割为固定大小的**page**
* 每个 page 都可以整个使用，或者分割为相同大小的**chunk**来使用
* 每个 cluster 都包含整数个 page，不同的 page 可以被划分为不同规格的 chunk
* 当需要从`njs_mp_t`中分配超过一个 page 大小的内存时，分配操作在 cluster 之外进行
* cluster 的地址和大块（超过一个 page）的起始地址都存放在一颗红黑树中，节点按照起始地址排序

其中提到了 chunk、page 和 cluster 这三类实体，首先得明白它们之间是什么关系以及是如何组织的，才能更好地理解`njs_mp_t`内存池的工作方式：

## chunk、page 和 cluster 这三者的关系

njs 里面的内存池名为`njs_mp_t`：

```c
typedef struct njs_mp_s  njs_mp_t;

struct njs_mp_s {
    /* rbtree of njs_mp_block_t. */
    njs_rbtree_t                blocks;

    njs_queue_t                 free_pages;

    uint8_t                     chunk_size_shift;
    uint8_t                     page_size_shift;
    uint32_t                    page_size;
    uint32_t                    page_alignment;
    uint32_t                    cluster_size;

    njs_mp_slot_t               slots[];
}
```

`mp`也就是 memory pool 的简称。它和`nginx_slab_pool_t`类似，采用的都是 best-fit 算法来查找空闲内存，为了支持快速查找，这里也把内存划分为几类固定大小的内存块的集合。在`njs_mp_t`中，这些固定大小的内存块被称为**chunk**，每一类内存块都在`slots`数组中占据一个槽，但是`njs_mp_slot_t`并不直接管理这些`slot`，chunk 由 page 划分而来，而 slot 则包含着具有该类规格的 chunk 的所有 page。

* `free_pages`：是一个链表，用于链接内存池中所有的空闲页。需要注意的是在 slot 中（也就是被分割成了 chunk 的 page 都是半满页），而全满页不在`free_pages`，也不再`slots`中。这点和`ngx_slab_pool_t`是一样的。
* `chunk_size_shift`：表示的是最小的 chunk 的大小（的移位量）

```c
typedef struct {
    njs_queue_t                 pages;

    /* Size of page chunks. */
    uint32_t                    size;

    /* Maximum number of free chunks in chunked page. */
    uint8_t                     chunks;
} njs_mp_slot_t;
```

* `pages`：链接这这个 slot 中所有的 page，前面已经提到 slot 中的 page 都是半满页
* `size`：表示这个 slot 中的 chunk 的规格（大小）
* `chunks`：表示这个 slot 中的一个 page 最多的可用空闲 chunk 数。这个数等于一个空闲页以该 slot 的 chunk 规格分割可以分割出来的 chunk 数目减一。这里只要记住，page 到了 slot，就肯定不是空闲页，所以至少有一个 chunk 被使用了。

```c
typedef struct {
    /*
     * Used to link pages with free chunks in pool chunk slot list
     * or to link free pages in clusters.
     */
    njs_queue_link_t            link;

    /*
     * Size of chunks or page shifted by mp->chunk_size_shift.
     * Zero means that page is free.
     */
    uint8_t                     size;

    /*
     * Page number in page cluster.
     * There can be no more than 256 pages in a cluster.
     */
    uint8_t                     number;

    /* Number of free chunks of a chunked page. */
    uint8_t                     chunks;

    uint8_t                     _unused;

    /* Chunk bitmap.  There can be no more than 32 chunks in a page. */
    uint8_t                     map[4];
} njs_mp_page_t;
```

`njs_mp_page_t`用来定义一个 page，这里说的 page 和我们在 OS 中说的 page 不是完全一回事，这里只是一个逻辑上的概念，其大小由外部参数指定。

* 首先 page 是属于 cluster 的，所以`number`来标记该 page 在所属 cluster 的序号
* 然后 page 是由 chunk 组成的，所以用`chunks`来标记该 page 当前包含的空闲 chunk 的数量
* 用`map`来标记该 page 中所有 chunk 的使用情况(哪些 free、哪些 busy)。
* 不同的 page 可能被划分为不同规格的 chunk，那么 chunk 的大小则有`size`来表示，如果 size 为 0，说明这是一个空闲页，还没有被分割为 chunk
* `link`用来链接其他的 page

```c
#define NJS_RBTREE_NODE(node)                                                 \
    njs_rbtree_part_t         node;                                           \
    uint8_t                   node##_color


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


typedef struct {
    NJS_RBTREE_NODE             (node);
    njs_mp_block_type_t         type:8;

    /* Block size must be less than 4G. */
    uint32_t                    size;

    u_char                      *start;
    njs_mp_page_t               pages[];
} njs_mp_block_t
```

介绍了 chunk 和 page，还剩下 cluster，但是并没有看到`njs_mp_cluster_t`结构，其实 cluster 只是一种`njs_mp_block_t`中，而 block 有多种类型，cluster 则是`NJS_MP_CLUSTER_BLOCK`类型。

我们知道对于小块内存，我们是从某个 cluster 中的某个 page 中分配出一个 chunk。而对于大块内存，每次分配我们都需要通过`njs_mp_t`向 OS 申请，而前面说了，`njs_mp_t`向 OS 申请内存是以 cluster 为单位，正确地说是以**block**为单位，cluster 只是 block 的一种。所以当 block 用于 chunk 这种小块内存分配时，它被称为 cluster。这样不论是管理 chunk 内存，还是大块内存，都可以用`njs_mp_block_t`，为二者提供了一致性借口，从而便于管理。

`njs_mp_block_t`中有一个`pages`柔性数组，所有的`njs_mp_page_t`都存储在其所属的 block 的这个数组中。前面说了`mp->free_pages`存储的是所有的空闲页，而`mp->slots`中存储的是所有的半满页，那么全满页显而易见还是存储在其所属的 block 中的。实际上不论是`mp->free_pages`还是`mp->slots`，它们都是对`njs_block_t`的`pages`数组中的元素的引用，只是进行了分类。这样的好处是，我们想要找一个空闲页时，不需要每个每个 block 地去找，只需要检查`mp->free_pages`。

其实`njs_mp_page_t`只是一个管理结构，并不持有真正的内存，向 OS 请求分配内存时分配的是一整块大内存，其地址由`start`字段指向。

为什么 block 需要用红黑树来串联起来呢？这是方便用一个内存地址找到其所属的 block。比如在回收内存的时候，传入一个`void *`，我们得用这个地址找到其所属的`njs_mp_block_t`，判断这是一个大块内存还是 chunk，如果是 chunk 则需要通过 block 找到其所属的 page，然后在 page 中回收这个 chunk。

## 总结

下面简单画了一张图：

![njs-memory-pool](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/njs/njs_memory_pool_overview.png)

可以看到 block 中的 page 有的是半满页，有点是空闲页（还有全满页满页画出来），对于半满页，block 中不同的 page 可以被分割成不同规格的 chunk。

为什么`njs_mp_t`内存池要分这么多中规格的实体呢？我猜测是为了更加精细地控制粒度.
