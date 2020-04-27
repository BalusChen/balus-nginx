# njs 数组

nginx base 自己就已经提供了许多数据结构，其中就包含动态数组`ngx_array_t`，所以我
搞不懂为什么 njs 又重新造轮子。想了一下可能有两个原因：

* njs 自己就可以是一个单独的命令行工具，所以不希望和 nginx base 有所关联
* nginx 中的数据结构都是为了特定的目的而精心设计的，可能和 njs 的需求不太一致

## 源码剖析

有了剖析 nginx base 的经验，我想对 njs 的基本数据结构的部分还是可以得心应手的。
不过得抓住重点，多想想为什么要这样设计。

### 结构体

```c
typedef struct {
    void              *start;
    /*
     * A array can hold no more than 65536 items.
     * The item size is no more than 64K.
     */
    uint16_t          items;
    uint16_t          avalaible;
    uint16_t          item_size;

    uint8_t           pointer;
    uint8_t           separate;
    njs_mp_t          *mem_pool;
} njs_arr_t;
```

除了`pointer`和`separate`这两个之外，大部分字段都很容易理解。那么这两个字段有什
么含义呢？这个最好要通过下面的函数来理解。

* `pointer`
* `separate`

### 创建、初始化和销毁

* `njs_arr_create`创建数组

```c
ngx_arr_t *
njs_arr_create(njs_mp_t *mp, njs_uint_t n, size_t size)
{
    njs_arr_t  *arr;
    
    arr = njs_mp_alloc(mp, sizeof(njs_arr_t) + n * size);
    if (njs_slow_path(arr == NULL)) {
        return NULL;
    }
    
    arr->start = (char *) arr + sizeof(njs_arr_t);
    arr->items = 0;
    arr->item_size = size;
    arr->avalaible = n;
    arr->pointer = 1;
    arr->separate = 1;
    arr->mem_pool = mp;
    
    return arr;
}
```

对于`njs_arr_create`需要注意的就是`njs_arr_t`本身和用来存储元素的`start`都是从堆
上分配的，这点和下面的`njs_arr_init`不同。

还有就是几个字段的设置，`pointer`字段设置为 1，是因为...;`separate`字段设置为 1
是因为...

* `njs_arr_init`初始化数组

```c
void *
njs_arr_init(njs_mp_t *mp, njs_arr_t *arr, void *start, njs_uint_t n,
    size_t size)
{
    arr->start = start;
    arr->items = n;
    arr->item_size = size;
    arr->avalaible = n;
    arr->pointer = 0;
    arr->separate = 0;
    arr->mem_pool = mp;
    
    if (arr->start == NULL) {
         arr->seperate = 1;
         arr->items = 0;

         arr->start = njs_mp_alloc(mp, n * size);
    }
    
    return arr->start;
}
```

这个函数是在一家创建好了的`njs_arr_t`的基础上进行初始化，传入的用来存储元素的
`start`中可能是已经有了元素的内存地址，也可能为空，为空的话，就需要进行分配内存，
但是如果分配失败也不管了。

这里注意一些字段的设置。

* 如果`start`上已经有元素，那么将`pointer`和`separate`设置为 0
* 如果`start`为空，那么将`pointer`设置为 0，`separate`设置为 1.

* `njs_arr_destroy`销毁数组

```c
void
njs_arr_destroy(njs_arr_t *arr)
{
    if (arr->separate) {
        njs_mp_free(arr->mem_pool, arr->start);
    }
    
    if (arr->pointer) {
        njs_mp_free(arr->mem_pool, arr);
    }
}
```

`njs_arr_t`结构体中最让人疑惑的就是`separate`和`pointer`这两个字段了，然而在
`njs_arr_destroy`我们可以对这两个字段的含义一窥端倪。

首先是`pointer`字段，只有在该字段被置位(为 1)的情况下才会销毁`njs_arr_t`结构体本
身，

### 添加、删除元素

* 添加元素

```c
void *
njs_array_add_multiple(njs_arr_t *arr, njs_uint_t items)
{
    void      *item, *start, *old; 
    uint32_t  n;
    
    n = arr->avalaible;
    items += arr->items;
    
    if (items >= n) {
    
        if (n < 16) {
            n *= 2;

        } else {
            n += n / 2;
        }
        
        if (n < items) {
            n = items;
        }
        
        start = njs_mp_alloc(arr->mp, n * arr->item_size);
        if (njs_slow_path(start == NULL)) {
            return NULL;
        }
        
        arr->avalaible = n;
        old = arr->start;
        arr->start = start;
        
        memcpy(start, old, (uint32_t) arr->items * arr->item_size);
        
        if (arr->separate == 0) {
            arr->separate = 1;
             
        } else {
            njs_mp_free(arr->mp, old);
        }
    }
    
    item = (char *) arr->start + (uint32_t) arr->items * arr->item_size;
    
    arr->items = items;
    
    return item;
}
```

首先是增长策略，如果容量不够就需要扩容。它的扩容策略有点像 MVSC 下`std::vector`
的扩容策略。在到达某个阈值(这里是 16) 之前，都是两倍扩容，到达阈值之后，就按 1.5
倍扩容。阈值设置为 16 我感觉太小了，但是这可能是因为`njs_arr_t`是用来存储大元素
的，所以元素个数就不会太多。

* 删除元素

```c
void
njs_arr_remove(njs_arr_t *arr, void *item)
{
    u_char    *next, *last, *end;
    uint32_t  item_size;
    
    item_size = arr->item_size;
    end = (u_char *) arr->start + item_size * arr->items;
    last = end - item_size;
    
    if (item != last) {
        next = (u_char *) item + item_size;
        
        memmove(item, next, end - next);
    }
    
    arr->items--;
}
```

方法很常规，就是把后面的元素往前移动一个位置，用的是`memove`，因为需要处理内存重
叠的情况。

## 和`ngx_array_t`进行比较

nginx base 同样提供了数组数据结构体`ngx_array_t`，这两者有什么不同呢？

## 总结

## 参考
