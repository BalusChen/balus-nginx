# nginx slab allocator（一）

## nginx slab 实现原理

nginx 的 slab 内存一般给多个进程通信用。进程间通信的方式有很多种，比如消息队列、共享内存等，这些 IPC 对于一般情况是可用的，但是如果进程之间需要交互各种大小不一的对象，需要共享复杂的数据结构，比如链表、红黑树等，那么这些 IPC 就难以支撑这么复杂的语义了。nginx 在共享内存的基础上，实现了一套 slab 内存机制，用以支持前面说的这些复杂的情况。

如何动态地管理内存呢，内存管理有两大难点：

1. 时间上，使用者会随机地申请分配、释放内存
2. 空间上，使用者每次申请分配的内存大小也是不一样的

能否有效地处理这两个问题是内存分配算法是否高效的关键，多次申请、释放不同大小的内存之后，很可能会造成内存碎片，从而造成内存浪费，拖慢分配过程。常见的内存分配算法有两个方向：first-fit 和 best-fit。所谓 first-fit，就是把找到的第一个大于等于所需内存块大小的内存块给分配出去；而 best-fit，则是寻找比所需内存块大的最少的内存块。first-fit 效率很高，best-fit 产生的内存块碎片更小。

nginx 的 slab 内存分配思路是基于 best-fit 的，也就是找到最合适的内存块。那么怎么高效地找到所需的内存块呢？

* nginx 首先假设模块使用 slab 分配内存通常需要的是小块内存，也就是小于一个 page 大小的内存。那么以 page 为基本单位将一整块内存分割。每个 page 只存放一种固定大小的内存块，内存块的大小以 2 的幂的形式从 8B 到 2KB 不等
* 由于每个 page 都只存放固定大小的一种内存块，所以 page 中内存块的个数是可数的，所以在 page 的最开始以 bitmap 的方式记录下每个内存块的使用情况，这样只通过遍历 bitmap 便可寻找到空闲内存块。
* 基于空间换时间的思想，slab allocator 会把外部请求的内存大小简化为有限的几种，前面说了内存块大小以 2 的幂从 8B 到 2KB 不等，那么如果外部请求的是 76B，那么也给他分配 128B 大小的内存块。
* 让有限的几种 page 链接成链表，且各个链表依序保存在数组中，类似哈希表，这样就可以用直接寻址法确定链表的位置。被分割成相同大小内存块的 page 以链表的形式链接在一起，8B-2KB 这些链表头部按序放在数组中（nginx 中称为`slots`数组）；这样外部内存分配请求来了，比如 63B，那么需要分配 64B 的内存块出去，在`[8B, 2KB]`中，63B 处于第 4 个位置，所以确定是在`slots[3]`这个链表中，遍历即可。
* nginx slab 将所有 page 分为**空闲页**、**半满页**和**全满页**这三种。空闲页并不会被分割，比如一个内存块为 64B 的半满页成为空闲页，后续它可以被分割成任何大小的内存块(8B-2KB)，而不仅仅只局限于 64B；所有的空闲页链接成一个链表。只有半满页，才根据不同的内存块大小链接成不同的链表，放在。全满页不参与链接。

### nginx slab allocator 结构

nginx 中的 slab allocator 以内存池`ngx_slab_pool_t`的形式存在：

```c
typedef struct {
    ngx_shmtx_sh_t    lock;

    size_t            min_size;
    size_t            min_shift;

    ngx_slab_page_t  *pages;
    ngx_slab_page_t  *last;
    ngx_slab_page_t   free;

    ngx_slab_stat_t  *stats;
    ngx_uint_t        pfree;

    u_char           *start;
    u_char           *end;

    ngx_shmtx_t       mutex;

    u_char           *log_ctx;
    u_char            zero;

    unsigned          log_nomem:1;

    void             *data;
    void             *addr;
} ngx_slab_pool_t;
```

  先解释几个字段：

* `min_size`：前面说了内存块有一个最小值，就是用这个字段来表示，不过在 nginx 中设置为 8B 了
* `min_shift`：shift 指的是对应的 size 的幂。比如 8 的 shift 就是 3。在确定位置时这个字段很有用，比如外部需要分配 63B，那么内部换算成 64B，其对应的 shift 为 6，那么在 slots 数组中的位置就是 6-3=3
* `addr`：slab 通常是和 zone 一起用的，这个字段对应着`ngx_shm_zone_t`中的`addr`字段（TODO：为什么需要这个字段？）
* `log_nomem`：顾名思义，控制在没有内存时是否需要记录日志
* `log_ctx`：在记录和 slab 相关的日志时，为了区别具体是哪个 slab 的日志，可以在 slab 中分配一段内存存放一串字符用于描述这个 slab，然后`log_ctx`就指向它
* `zero`：零字符`\0，` `log_ctx`默认就指向它

![nginx-slab-allocator](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/a6c24b023a4b21ec0526668346903e0cc4d02765/Nginx/slab/nginx-slab-allocator.png)

可以看到所有和 slab 相关的数据都存储在一整块内存当中，而这块内存被分为 6 个部分：

* `ngx_slab_pool_t`结构体本身：相当于一个控制头部。
* `slots`数组：所有**半满页**的描述结构`ngx_slab_page_t`按照内存块大小链接成的链表组成的数组，这块区域在`ngx_slab_pool_t`结构体中没有专门的字段指向，其实也不需要，知道`ngx_slab_pool_t`的地址，加上`sizeof(ngx_slab_pool_t)`就得到了 slots 数组的位置。
* `stats`数组：这个数组和`slots`数组是一一对应的，用于记录下每个 slot 的内存分配情况。
* `pages`数组：对于每个 page，都有一个`ngx_slab_page_t`的结构用于描述这个 page，所有的描述结构组成了这个数组
* `padding`：后面就是所有 page 内存了。为了让 page 都以(TODO：多少)内存对齐，可能需要一些填充字节。
* 最后面就是所有 page 实际所在的内存
