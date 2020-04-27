# Nginx链表

## 链表的结构

首先来看看Nginx中链表的结构`ngx_list_t`：

里面的`ngx_list_part_t`就是链表元素，其结构如下：

```c
typedef struct ngx_list_part_s ngx_list_part_t;

struct ngx_list_part_s {
    void *elts;
    ngx_uint_t nelts;
    ngx_list_part_t *next;
};
```

* `elts`: 用于实际放置元素的内存(一个数组)的起始地址
* `nelts`: 数组中已有元素的个数
* `next`: 链接到下一个节点

从`elts`和`nelts`可以看出:

`ngx_list_part_t`并不仅仅只是一个节点，而是节点数组。所以说Nginx中的链表和普通的链表还是有所不同的，它其实是数组的链表。

```c
typedef struct {
    ngx_list_part_t *last;
    ngx_list_part_t part;
    size_t size;
    ngx_uint_t nalloc;
    ngx_pool_t *pool;
} ngx_list_t;
```

* `last`: 链表中最后一个元素
* `part`: 表头
* `size`: 每个元素的最大大小
* `nalloc`: 每一个数组的元素的最大个数
* `pool`: 给链表分配内存的内存池

![nginx-list](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/Nginx/nginx-list.png)

## 操作

### 创建数组

```c
ngx_list_t *
ngx_list_create(ngx_pool_t *pool, ngx_uint_t n, size_t size)
{
    ngx_list_t *list;

    list = ngx_palloc(pool, sizeof(ngx_list_t));
    if (list == NULL) {
        return NULL;
    }

    if (ngx_list_init(list, pool, n, size) != NGX_OK) {
        return NULL;
    }

    return list;
}

```

### 插入元素

和Nginx其他许多数据结构的添加操作一样，`ngx_list_t`结构的添加操作是返回一个`ngx_list_part_t`结构，供用户自行填入内容。

```c
void *
ngx_list_push(ngx_list_t *l)
{
    void *elt;
    ngx_list_part_t *last;

    last = l->last;

    if (last->nelts == l->nalloc) {

        // 最后一个part满了，分配一个新的ngx_list_part_t结构
        last = ngx_palloc(l->pool, sizeof(ngx_list_part_t));
        if (last == NULL) {
            return NULL;
        }

        // 为ngx_list_part_t中的elts分配内存
        last->elts = ngx_palloc(l->pool, l->nalloc * l->size);
        if (last->elts == NULL) {
            return NULL;
        }

        last->nelts = 0;
        last->next = NULL;

        l->last->next = last;
        l->last = last;
    }

    // 返回可用的内存给调用者，让其自行填入内容
    elt = (char *) last->elts + l->size * last->nelts;
    last->nelts++;

    return elt;
}
```

代码很简单，我们可以看到，每次插入元素都是插入到`last`节点(数组)中去。如果它满了，就新分配一块内存并更新`last`。而且我们可以看到，每个元素都只给`size`大小。

### 遍历链表

Nginx中没有特地给出遍历操作(LevelDB里面处处是遍历)，但是在源文件的注释中给出了一段代码用以遍历，我把它抄写在这里。

```c
part = &list.part;
data = part->elts;

for (i = 0; ; i++) {
    if (i >= part->nelts) {
        if (part->next == NULL) {
            /* 遍历完了所有链表 */
            break;
        }

        part = part->next;
        data = part->elts;
        i = 0;
    }
    /* do something */

}
```
