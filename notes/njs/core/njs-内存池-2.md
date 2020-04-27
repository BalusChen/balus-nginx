# njs 内存池的创建与销毁

## 源码剖析

### 内存池的创建

```c
njs_mp_t *
njs_mp_create(size_t cluster_size, size_t page_alignment, size_t page_size,
    size_t min_chunk_size)
{
    if (njs_slow_path(!njs_is_pow_of_two(page_alignment)
                      || !njs_is_pow_of_two(page_size))
                      || !njs_is_pow_of_two(min_chunk_size))
    {
        return NULL;
    }
    
    page_alignment = njs_max(page_alignment, NJS_MAX_ALIGNMENT);
    
    if (njs_slow_path(page_size < 64
                     || page_size < page_alignment)
                     || page_size < min_chunk_size
                     || min_chunk_size * 32 < page_size
                     || cluster_size < page_size
                     || cluster_size / page_size > 256
                     || cluster_size % page_size != 0)

    {
        return NULL;
    }
    
    return njs_mp_fast_create(cluster_size, page_alignment, page_size,
                              min_chunk_size);
}
```

`njs_mp_create`没有做什么实质性的工作，只是对参数进行了有效性检查。实际的内存池
创建操作是由`njs_mp_fast_create`完成的，但是从上面这个函数中我们也可以看出内存池
所具有的性质。

首先页面大小和页面对齐量需要是 2 的幂，而且页面不能太小，必须大于 64 字节。

然后 cluster 的必须是页面的整数倍(但是不一定得要 2 的幂)，1 个 cluster 最多包含
256 个 page。这个可以从`njs_mp_page_t`结构体中的`number`字段看出来，这个字段表示
该 page 在 cluster 中的序号，由于它是 8-bit，所以最多 255 个号码，所以一个 cluster
最多 256 个 page。

最后就是 page 和 chunk 的关系了。一个 page 包含最多 32 个 chunk，这个性质可以从
`njs_page_t`中的`map`位图中可以看出，它一个有 32 bit，每个 bit 用来表示该 page
中的一个 chunk。

```c
njs_mp_t *
njs_mp_fast_create(size_t cluster_size, size_t page_alignment, size_t page_size,
    size_t min_chunk_size)
{
    njs_mp_t       *mp;
    njs_uint_t     slots, chunk_size;
    njs_mp_slot_t  *slot;
    
    slots = 0;
    chunk_size = page_size;
    
    do {
        slots++;
        chunk_size /= 2;
    } while (chunk_size > min_chunk_size);
    
    mp = njs_zalloc(sizeof(njs_mp_t) + slots * sizeof(njs_mp_slot_t));
    
    if (njs_fast_path(mp != NULL)) {
        mp->page_size = page_size;
        mp->page_alignment = njs_max(page_alignment, NJS_MAX_ALIGNMENT);
        mp->cluster_size = cluster_size;
        
        slot = mp->slots;
        
        do {
            njs_queue_init(slot->pages);
            
            slot->chunk_size = chunk_size;
            slot->chunks = (page_size / chunk_size) - 1;
            
            slot++;
            chunk_size *= 2;
        } while (chunk_size < page_size);
        
        mp->chunk_size_shift = njs_mp_shift(min_chunk_size);
        mp->page_size_shift = njs_mp_shift(page_size);
        
        njs_rbtree_init(mp->blocks, njs_mp_rbtree_compare);
        
        njs_queue_init(&mp->free_pages);
    }
    
    return mp;
}
```

从这个函数我们大概可以才想到`njs_mp_t`是一个什么样的结构。

首先我们知道`njs_mp_t`中的`slots`字段是一个柔性数组(flexible array)，所以
使用`njs_zalloc`为`njs_mp_t`分配内存时还需要分配额外的内存。

我们知道`njs_mp_t`就是一个 cluster，而每个 cluster 包含多个 page，每个 page 由
一系列同样大小的 chunk 组成。但是 cluster 中的每个 page 却是由不同大小的 chunk 组
成的，这个和 OS 中内存分配算法中的

由于`min_chunk_size`和`page_size`都是 2 的幂，所以`page_size`一定是 `min_chunk_size`
的 2 的幂次倍。

然后两个`xxx_shift`字段有什么用呢？

#### 举个例子

光看代码不太好理解，举个例子，假设 `page_size` 为 4K，`min_chunk_size`为 512。那
么分配出来的内存池的结构就是这样的：

![mp-example]()

首先第一个`do-while`循环算出来一共需要 4K/512 - 1 = 7 个 page，也就是`slots = 7`，然后
在第二个`do-while`循环中为确定每个 page 中包含的 chunk 的大小，大小分别为
`mp->slots[0].chunk_size = 512`, `mp->slots[1].chunk_size = 1K` ...`mp->slots[6].chunk_size == 4K`。

然后就是`chunk_size_shift`和`page_size_shift`这两个字段了。

### 内存池的销毁

## 总计

## 参考
