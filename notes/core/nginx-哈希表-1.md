# Nginx哈希表

## 定义

```c
typedef struct {
    void *value;
    u_short len;
    u_char name[1];
} ngx_hash_elt_t;


typedef struct {
    ngx_hash_elt_t **buckets;
    ngx_uint_t size;
} ngx_hash_t;

typedef struct {
    ngx_str_t     key;
    ngx_uint_t    key_hash;
    void         *value;
} ngx_hash_key_t;
```

## 初始化

函数原型如下:

```c
ngx_int_t
ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names, ngx_uint_t nelts)
```

在看该函数的具体实现之前，先看看`ngx_hash_init_t`是个什么东东：

```c
typedef struct {
    ngx_hash_t *hash;
    ngx_hash_key_pt key;

    ngx_uint_t max_size;
    ngx_uint_t bucket_size;

    char *name;
    ngx_pool_t *pool;
    ngx_pool_t *temp_pool;
} ngx_hash_init_t;
```

* `hash`: 如果为`NULL`，则调用完初始化函数之后，该字段指向新创建出来的hash表；否则(不为空)，则在初始化时，所有数据都被插入到它指向的hash表中。
* `key`: hash函数
* `max_size`: hash表中桶的个数
* `bucket_size`: 每个桶的最大限制大小。如果在初始化时发现有的数据放不下，则初始化失败。
* `name`: 该hash表的名字
* `pool`: 为该hash表分配内存的pool
* `temp_pool`: TODO

然后是`ngx_hash_key_t`:

```c
typedef struct {
    ngx_str_t key;
    ngx_uint_t key_hash;
    void *value;
} ngx_hash_key_t;
```

很简单的一个表，各个字段的作用一目了然。

现在来看看其具体实现：

```c
ngx_int_t
ngx_hash_init(ngx_hash_init_t *hinit, ngx_hash_key_t *names, ngx_uint_t nelts)
{
    u_char *elts;
    size_t len;
    u_short *test;
    ngx_uint_t i, n, key, size, start, bucket_size;
    ngx_hash_elt_t *elt, **buckets;

    // 桶的大小不能为0，不然一个元素都放不下
    if (hinit->max_size == 0) {
        ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                      "could not build %s, you should "
                      "increase %s_max_size: %i",
                      hinit->name, hinit->name, hinit->max_size);
        return NGX_ERROR;
    }

    // 检查每个桶是否够存储一个关键字元素
    for (n = 0; n < nelts; n++) {
        // NOTE：这里需要注意理解
        // Nginx中的hash表的每个bucket并不是真正的链表，而是一段连续的内存
        // bucket直接以一个void*指针来分隔
        // 这个指针可以作为查找结束的标志
        if (hinit->bucket_size < NGX_HASH_ELT_SIZE(&names[n]) + sizeof(void *))
        {
            ngx_log_error(NGX_LOG_EMERG, hinit->pool->log, 0,
                          "could not build %s, you should "
                          "increase %s_bucket_size: %i",
                          hinit->name, hinit->name, hinit->bucket_size);
            return NGX_ERROR;
        }
    }

    // TODO: test是个什么东西
    // test中的每个元素会累计落到hash表该位置上的关键字长度
    // 这里使用的是malloc而不是从内存池中分配，因为test是临时的，用完就释放
    test = ngx_alloc(hinit->max_size * sizeof(u_short), hinit->pool->log);
    if (test == NULL) {
        return NGX_ERROR;
    }

    // bucket以void*结尾，这里求得每个bucket的实际可用大小
    bucket_size = hinit->bucket_size - sizeof(void *);

    // start表示大概会有多少个bucket
    // TODO: 这里bucket_size/(2*sizeof(void*))，为什么要这样算呢?
    // 之所以取名为start是因为Nginx中hash表的桶的个数不是一开始就设置为hinit->max_size
    // 而是从start开始一直探测到hinit->max_size，直到找到合适的大小(TODO: 合适的条件是什么后面会提到)
    start = nelts / (bucket_size / (2 * sizeof(void *)));
    // start至少为1，因为至少要有一个bucket(不然hints->hash->buckets就为空了?TODO)
    start = start ? start : 1;

    // TODO: 不知道这几个数字是怎么得来的
    if (hinit->max_size > 10000 && nelts && hinit->max_size / nelts < 100) {
        start = hinit->max_size - 1000;
    }

    // 从start开始，一直探测到hinit->max_size
    for (size = start; size <= hinit->max_size; size++) {

        ngx_memzero(test, size * sizeof(u_short));

        for (n = 0; n < nelts; n++) {
            if (names[n].key.data == NULL) {
                continue;
            }

            // 求得在桶的个数为size时该元素所防止的桶的下标
            key = names[n].key_hash % size;
            // TODO: 为什么不使用+=?
            test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));

#if 0
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %ui %ui \"%V\"",
                          size, key, test[key], &names[n].key);
#endif

            // NOTE: 这里就是”合适“的条件所在处了
            // 必须满足在size个桶的情况下每个桶的大小都比bucket_size小，才算找到了合适的大小
            if (test[key] > (u_short) bucket_size) {
                goto next;
            }
        }

        goto found;

    next:

        continue;
    }

    // 遍历到了hinit->max_size都没有找到合适的大小，就报告错误
    size = hinit->max_size;

    ngx_log_error(NGX_LOG_WARN, hinit->pool->log, 0,
                  "could not build optimal %s, you should increase "
                  "either %s_max_size: %i or %s_bucket_size: %i; "
                  "ignoring %s_bucket_size",
                  hinit->name, hinit->name, hinit->max_size,
                  hinit->name, hinit->bucket_size, hinit->name);

found:
    /* 找到合适的大小之后进行的一系列工作 */

    // 前两个for循环首先计算每个bucket所需的大小
    // 存储在test数组中
    for (i = 0; i < size; i++) {
        test[i] = sizeof(void *);
    }

    for (n = 0; n < nelts; n++) {
        // TODO: 为什么会传入NULL的data呢？
        if (names[n].key.data == NULL) {
            continue;
        }

        key = names[n].key_hash % size; // 在哪个桶
        test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));
    }

    // 计算总的大小
    // NOTE: 这里和前面的大小有点不一样，主要是因为Nginx要进行对齐
    len = 0;

    for (i = 0; i < size; i++) {
        // test[i]的大小为0说明该bucket无需存放任何元素，因为每个bucket至少有一个void*作为bucket之间的分隔
        if (test[i] == sizeof(void *)) {
            continue;
        }

        test[i] = (u_short) (ngx_align(test[i], ngx_cacheline_size));

        len += test[i];
    }

    // 如果传入的哈希表为NULL，则重新分配
    if (hinit->hash == NULL) {
        hinit->hash = ngx_pcalloc(hinit->pool, sizeof(ngx_hash_wildcard_t)
                                             + size * sizeof(ngx_hash_elt_t *));
        if (hinit->hash == NULL) {
            // 注意这里，我们是使用malloc分配的test，而不是从内存池中获取，所以需要及时free
            ngx_free(test);
            return NGX_ERROR;
        }

        buckets = (ngx_hash_elt_t **)
                      ((u_char *) hinit->hash + sizeof(ngx_hash_wildcard_t));

    } else {
        // 为每个桶(的首地址)分配内存
        buckets = ngx_pcalloc(hinit->pool, size * sizeof(ngx_hash_elt_t *));
        if (buckets == NULL) {
            ngx_free(test);
            return NGX_ERROR;
        }
    }

    // TODO: 为什么还要加ngx_cacheline_size？前面不是已经对齐了么？
    elts = ngx_palloc(hinit->pool, len + ngx_cacheline_size);
    if (elts == NULL) {
        ngx_free(test);
        return NGX_ERROR;
    }

    // 获取对齐的内存首地址
    elts = ngx_align_ptr(elts, ngx_cacheline_size);

    for (i = 0; i < size; i++) {
        if (test[i] == sizeof(void *)) {
            continue;
        }

        // 记录下每个桶的首地址
        buckets[i] = (ngx_hash_elt_t *) elts;
        elts += test[i];
    }

    // TODO: 开始
    for (i = 0; i < size; i++) {
        test[i] = 0;
    }

    for (n = 0; n < nelts; n++) {
        if (names[n].key.data == NULL) {
            continue;
        }

        key = names[n].key_hash % size;
        elt = (ngx_hash_elt_t *) ((u_char *) buckets[key] + test[key]);

        elt->value = names[n].value;
        elt->len = (u_short) names[n].key.len;

        // 转换成小写的形式存储在name中
        ngx_strlow(elt->name, names[n].key.data, names[n].key.len);

        test[key] = (u_short) (test[key] + NGX_HASH_ELT_SIZE(&names[n]));
    }

    for (i = 0; i < size; i++) {
        if (buckets[i] == NULL) {
            continue;
        }

        elt = (ngx_hash_elt_t *) ((u_char *) buckets[i] + test[i]);

        elt->value = NULL;
    }

    ngx_free(test);

    hinit->hash->buckets = buckets;
    hinit->hash->size = size;

#if 0

    for (i = 0; i < size; i++) {
        ngx_str_t val;
        ngx_uint_t key;

        elt = buckets[i];

        if (elt == NULL) {
            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: NULL", i);
            continue;
        }

        while (elt->value) {
            val.len = elt->len;
            val.data = &elt->name[0];

            key = hinit->key(val.data, val.len);

            ngx_log_error(NGX_LOG_ALERT, hinit->pool->log, 0,
                          "%ui: %p \"%V\" %ui", i, elt, &val, key);

            elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                                   sizeof(void *));
        }
    }

#endif

    return NGX_OK;
}
```

下面看一下`ngx_hash_init`函数中调用的其他一些函数或者宏

* `NGX_HASH_ELT_SIZE`宏:

```c
#define ngx_align(d, a) (((d) + (a - 1)) & ~(a - 1))

// 计算出ngx_hash_elt_t结构的大小
// +2是因为该结构中的len字段
// TODO: 以void*大小对齐是因为啥？
#define NGX_HASH_ELT_SIZE(name) \
    (sizeof(void *) + ngx_align((name)->key.len + 2, sizeof(void *)))
```

我还有几个问题：

1. 为什么传给`ngx_hash_init`函数的`name`参数是`ngx_hash_key_t`而不是`ngx_hash_elt_t`

## 查找

```c
void *
ngx_hash_find(ngx_hash_t *hash, ngx_uint_t key, u_char *name, size_t len)
{
    ngx_uint_t i;
    ngx_hash_elt_t *elt;

#if 0
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "hf:\"%*s\"", len, name);
#endif

    // 找到对应的桶
    elt = hash->buckets[key % hash->size];

    // NOTE: 每个桶以NULL结尾
    if (elt == NULL) {
        return NULL;
    }

    while (elt->value) {
        if (len != (size_t) elt->len) {
            goto next;
        }

        for (i = 0; i < len; i++) {
            if (name[i] != elt->name[i]) {
                goto next;
            }
        }

        return elt->value;

    next:

        // 前进到桶中下一个元素的位置
        // 注意这里需要对其
        elt = (ngx_hash_elt_t *) ngx_align_ptr(&elt->name[0] + elt->len,
                                               sizeof(void *));
        continue;
    }

    return NULL;
}
```

## 参考资料

[ngx_hash_init分析](http://www.voidcn.com/article/p-njnbfvxl-o.html)

[看云: nginx哈希表结构](https://www.kancloud.cn/digest/understandingnginx/202593)

[GitHub: NGINX源码剖析](https://github.com/y123456yz/reading-code-of-nginx-1.9.2/blob/master/nginx-1.9.2/src/core/ngx_hash.c)
