# Nginx基数树

## 结构定义

```c
#define NGX_RADIX_NO_VALUE (uintptr_t) -1

typedef struct ngx_radix_node_s ngx_radix_node_t;

struct ngx_radix_node_s {
    ngx_radix_node_t *right;
    ngx_radix_node_t *left;
    ngx_radix_node_t *parent;
    uintptr_t value;
};

typedef struct {
    ngx_radix_node_t *root;
    ngx_pool_t *pool;
    ngx_radix_node_t *free;
    char *start;
    size_t size;
} ngx_radix_tree_t;
```

* `root`: 也就是基数树的根节点了
* `pool`: 为这颗树分配内存的内存池
* `free`: 从基数树中删除元素并不会真正地释放空间，而是将删除的节点挂在free链表上
* `start`: 为结点分配内存时，从该字段指向的内存地址开始分配
* `size`: `start`指向的内存剩余的大小

## 操作

### 创建一棵树

* 首先为为树结构和root节点分配内存

```c
ngx_radix_tree_t *
ngx_radix_tree_create(ngx_pool_t *pool, ngx_int_t preallocate)
{
    uint32_t key, mask, inc;
    ngx_radix_tree_t *tree;

    tree = ngx_palloc(pool, sizeof(ngx_radix_tree_t));
    if (tree == NULL) {
        return NULL;
    }

    tree->pool = pool;
    tree->free = NULL;
    tree->start = NULL;
    tree->size = 0;

    // 为根节点分配内存
    tree->root = ngx_radix_alloc(tree);
    if (tree->root == NULL) {
        return NULL;
    }
    tree->right = NULL;
    tree->left = NULL;
    tree->parent = NULL;
    tree->value = NGX_RADIX_NO_VALUE; // 设置初始值，表示这个位置是空的
```

* 而后，`preallocate`参数决定如何为进行预分配
    * 如果为0，则不额外分配节点
    * 如果为-1，则根据页面大小来决定如何预分配
    * 否则直接按照其值来分配

```c
    if (preallocate == 0) {
        return tree;
    }

    if (preallocate == -1) {
        switch (ngx_pagesize / sizeof(ngx_radix_node_t)) {
        case 128:
            preallocate = 6;
            break;
        case 256:
            preallocate = 7;
            break;
        default:
            preallocate = 8;
        }
    }
```

在代码中还有一段注释，里面有几句话:

> There is no sense to preallocate more than one page, because further preallocation distributes the only bit per page. Instead, a random insertion may distribute several bits per page.

TODO: 暂时还不太懂这句话

* 然后预分配节点

```c
    mask = 0;
    inc = 0x80000000;

    while (preallocate--) {
        key = 0;
        mask >>= 1;
        mask |= 0x80000000;

        do {
            if (ngx_radix32tree_insert(tree, key, mask, NGX_RADIX_NO_VALUE)
                != NGX_OK)
            {
                return NULL;
            }

            key += inc;

        } while (key);

        inc >>= 1;
    }

    return tree;
}
```

刚开始看的时候觉得这个`while`循环很难，看不懂。后来我把`do-while`里头的插入语句变成`printf`:

```c
        do {
            printf("mask = 0x%x\n", mask);
            printf("key = 0x%x\n", key);
            printf("inc = 0x%x\n\n", inc);

            key += inc;

        } while (key);

        inc >>= 1;

        printf("====================\n");
    }
```

通过观察其输出就知道它是怎么工作的了。

首先假设`preallocate`为6。
一开始`mask`为`0x80000000`，只有最高位为1，说明是插入到第一层(root为第0层)，
`inc`为`0x80000000`，在`do-while`循环中，开始key为

### 插入元素

* 首先查找可用位置:

```c
ngx_int_t
ngx_radix32tree_insert(ngx_radix_tree *tree, uint32_t key, uint32_t mask,
    uintptr_t value)
{
    uint32_t bit;
    ngx_radix_node_t *node, *next;


    node = tree->root;
    next = tree->root;

    bit = 0x80000000;

    while (bit & mask) {
        // 在mask允许的范围内查找可用位置
        if (bit & key) { // 0左1右
            next = node->right;
        } else {
            next = node->left;
        }

        if (next == NULL) {
            break;
        }

        bit >>= 1; // 移动到下一层
        node = next;
    }
```

* 如果查找到可用位置:

```c
    if (next) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            return NGX_BUSY; // 该位置上已经有元素了
        }

        node->value = value;
        return NGX_OK;
    }
```

* 如果路径不够:

```c
    while (bit & mask) {
        next = ngx_radix_alloc(tree);
        if (next == NULL) {
            return NGX_ERROR;
        }

        next->right = NULL;
        next->left = NULL;
        next->parent = node;
        next->value = NGX_RADIX_NO_VALUE;

        if (bit & key) {
            node->right = next;
        } else {
            node->left = next;
        }

        bit >>= 1;
        node = next;
    }

    node->value = value;

    return NGX_OK;
}
```

### 删除元素

删除元素分为两步:

* 找到要删除的节点:

```c
ngx_int_t
ngx_radix32tree_delete(ngx_radix_tree_t *tree, uint32_t key, uint32_t mask)
{
    uint32_t bit;
    ngx_radix_node_t *node;

    bit = 0x80000000;
    node = tree->root;

    while (node && (bit & mask)) {
        if (bit & key) {
            node = node->right;
        } else {
            node = node->left;
        }

        bit >>= 1; // 转到下一层
    }

    if (node == NULL) {
        // 没有找到
        return NGX_ERROR;
    }

```

* 回收节点及其到root路径上的无用节点:

```c
    for ( ;; ) {
        if (node == node->parent->right) {
            node->parent->right = NULL;
        } else {
            node->parent->left = NULL;
        }

        // 将要回收的节点挂在free链表上
        node->right = tree->free;
        tree->free = node;

        node = node->parent;

        if (node->left || node->right) {
            // 对于路径上的节点，只要其左右子树不是全为空,那么就不能将其删除，
            // 因为沿着不为空的子树向下到达的叶节点上还有值(当然也可能是预分配时分配额NGX_RADIX_NO_VALUE)
            break;
        }

        if (node->value != NGX_RADIX_NO_VALUE) {
            // TODO: 这里不太懂，路径上的节点(树内部节点)不应该都是NGX_RADIX_NO_VALUE么?
            break;
        }

        if (node->parent == NULL) {
            // root节点不回收
            break;
        }
    }
}
```

### 查找

```c
ngx_int_t
ngx_radix32tree_find(ngx_radix_tree_t *tree, uint32_t key)
{
    uint32_t bit;
    uintptr_t value;
    ngx_radix_node_t *node;

    bit = 0x80000000;
    node = tree->root;

    while (root) {
        if (node->value != NGX_RADIX_NO_VALUE) {
            value = node->value;
        }

        if (bit & key) {
            node = node->right;
        } else {
            node = node->left;
        }

        bit >>= 1;
    }

    return value;
}
```

了解了基数树的原理，理解这个查找就没有多大的问题了。
但是有点不同的是，这里无论如何都会返回一个值，比如要找的`key`为0xFF000000，但是树只有4层，那么只会安装0xF0000000来查找，还是有些特别的。

### 内存分配

前面对于`ngx_radix_tree_t`结构还有`start`和`size`两个字段没有提及。
它们的作用在这个结点分配函数中就体现出来了:


```c
static ngx_radix_node_t *
ngx_radix_alloc(ngx_radix_tree_t *tree)
{
    ngx_radix_node_t *p;

    // 首先从free链表里查询是否有可利用的结点
    if (tree->free) {
        p = tree->free;
        tree->free = tree->free->right;
        return p;
    }

    // 剩余的内存不够分配一个节点
    // 就再分配一个页面
    if (tree->size < sizeof(ngx_radix_node_t)) {
        tree->start = ngx_pmemalign(tree->pool, ngx_pagesize, ngx_pagesize);
        if (tree->start == NULL) {
            return NULL;
        }

        tree->size = ngx_pagesize;
    }

    // 从start指向的内存开始进行内存分配
    p = (ngx_radix_node_t *) start;
    tree->start += sizeof(ngx_radix_node_t);
    tree->size -= sizeof(ngx_radix_node_t);
}
```

可以看出，对于基数树的内存分配:

1. 查看`free`链表中是否有可重复利用的节点,如果有则直接返回，否则转到2
2. 看看为这棵树分配的内存还剩多少(`size`字段)，足够则从`start`指向的内存开始分配，否则转到3
3. 为这棵树继续分配一个页面大小的内存，`start`指向这块内存的首地址，然后从`start`中分配一个结点

## 总结

以前虽然听过基数树，但是不知道它的具体原理。
这次通过阅读NGINX源码，不仅让我知道了其原理，还知道了如何实现。

而且NGINX的源码里面有两个特殊的数字: 32和128，看到它们我马上想起了IPv4和IPv6地址，我觉得这个数据结构可能是为它们准备的(或者至少是它们用得上)。


### 问题

阅读源码好像每次都会有一点问题，以前不会太过重视，但是经过与别人交流，发现很多理解上的偏差正是由于我没有搞懂这些问题，所以我打算将其记录下来，虽然现在不一定能够理解，但是我觉得经过反复地阅读代码、查阅资料，一定会有理解之时。


1. 为什么不直接使用`ngx_radix_tree_t`中的`pool`字段内存池来进行结点内存的分配？而是搞一个`start`和`size`？

## 参考

