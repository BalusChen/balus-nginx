# Nginx队列

## 定义

```c
typedef struct ngx_queue_s ngx_queue_t;

struct ngx_queue_s {
    ngx_queue_t *prev;
    ngx_queue_t *next;
};
```

### 疑惑

我有点奇怪，上面只有两个指针，但是怎么存数据呢？这个问题困扰了了我很久，直到我看了《深入理解NGINX》(实在想不出就看答案🤢)。
其实`ngx_queue_t`只是起一个链接的作用，它可以作为一个结构体的成员，然后通过`ngx_queue_data`宏来获取该结构体的指针:

```c
#define ngx_queue_data(q, type, link) \
    (type *) ((u_char *) q - offsetof(type, link))
```

其中宏的参数作用如下:

* `q`: `ngx_queue_t`变量
* `type`: `ngx_queue_t`所在结构的结构名
* `link`: `ngx_queue_t`在该结构中的字段名

### 容器与元素

头文件中提供了许多`ngx_queue_XXX`宏，不过它们其实是分为两部分的。比如`ngx_queue_head`是获取
队列的头指针，而`ngx_queue_next`是获取当前节点的下一个节点；前者是针对队列这个容器的操作，而后者则是针对容器中某一元素的操作。

#### 针对容器的操作

* `ngx_queue_init`
* `ngx_queue_empty`
* `ngx_queue_insert_head`
* `ngx_queue_insert_tail`
* `ngx_queue_head`
* `ngx_queue_last`
* `ngx_queue_sentinel`
* `ngx_queue_remove`
* `ngx_queue_split`
* `ngx_queue_add`
* `ngx_queue_middle`
* `ngx_queue_sort`

#### 针对元素的操作

* `ngx_queue_next`
* `ngx_queue_prev`
* `ngx_queue_data`
* `ngx_queue_insert_after`

### 结构

![ngx-queue-empty](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/Nginx/ngx-queue-1-empty-queue.png)

![ngx-queue-1-element](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/Nginx/ngx-queue-1-element.png)

![ngx-queue-2-elements](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/Nginx/ngx-queue-2-elements.png)

## 操作

只有理解了**容器**与**元素**的区别，才不会被`ngx_queue_XXX`宏的各个参数搞晕。

### 取中间元素

Nginx的队列中有一个取中间元素的操作(虽然我不知道有什么用)，
用的是很常见的(其实在刷leetcode之前我没有见过，但是我觉得我菜，别人应该很多都知道，所以就这样说)快慢指针来做的

```c
/*
 * 如果队列长度为奇数，则返回中间元素
 * 若为偶数，则返回第二部分的第一个元素
 */
ngx_queue_t *
ngx_queue_middle(ngx_queue_t *queue)
{
    ngx_queue_t *middle, *next;

    middle = ngx_queue_head(queue);

    // 队列为空
    if (middle == ngx_queue_last(queue)) {
        return middle;
    }

    next = ngx_queue_head(queue);

    for ( ;; ) {
        middle = ngx_queue_next(middle);

        next = ngx_queue_next(next);

        if (next == ngx_queue_last(queue)) {
            return middle;
        }

        next = ngx_queue_next(next);

        if (next == ngx_queue_last(queue)) {
            return middle;
        }
    }
}
```

### 排序

队列中还提供了一个排序操作。对于链表的排序，可以使用归并，也可以使用插入，NGINX选择的是插入排序。

```c
void
ngx_queue_sort(ngx_queue_t *queue,
    ngx_int_t (*cmp)(const ngx_queue_t *, const ngx_queue_t *))
{
    ngx_queue_t *q, *prev, *next;

    q = ngx_queue_head(queue);

    // 元素个数为1或者0
    if (q == ngx_queue_last(queue)) {
        return;
    }

    /*
     * 很经典的插入排序实现
     * 从第二个节点开始，向前找第一个比它小的节点，插入到它后面
     */
    for (q = ngx_queue_next(q); q != ngx_queue_sentinel(queue); q = next) {

        prev = ngx_queue_prev(q);
        next = ngx_queue_next(q);

        ngx_queue_remove(q);

        do {
            // 找到第一个<=q的节点
            if (cmp(prev, q) <= 0) {
                break;
            }

            prev = ngx_queue_prev(prev);

        } while (prev != ngx_queue_sentinel(queue));

        // 插入到这个(第一个)比q小的节点的后面
        ngx_queue_insert_after(prev, q);
    }
}
```

## 示例

```c
typedef struct {
    char *str;
    ngx_queue_t link;
    int num;
} my_struct_t;

int
main(int argc, char **argv)
{
 my_struct_t arr[6];
 ngx_queue_t queue_container;
 ngx_queue_t *q;
    my_struct_t *tp;

 ngx_queue_init(&queue_container);
 for (int i = 0; i < 6; i++) {
     arr[i].num = i;
 }
 ngx_queue_insert_tail(&queue_container, &arr[0].link);
 ngx_queue_insert_head(&queue_container, &arr[1].link);
 ngx_queue_insert_tail(&queue_container, &arr[2].link);
 ngx_queue_insert_after(&queue_container, &arr[3].link);
 ngx_queue_insert_tail(&queue_container, &arr[4].link);
 ngx_queue_insert_tail(&queue_container, &arr[5].link);


#if 1
 for (q = ngx_queue_head(&queue_container);
     q != ngx_queue_sentinel(&queue_container);
     q = ngx_queue_next(q)) {
     tp = ngx_queue_data(q, my_struct_t, link);
     printf("%d ", tp->num);
 }
#endif
    printf("\n");

    for (q = ngx_queue_last(&queue_container);
        q != ngx_queue_sentinel(&queue_container);
        q = ngx_queue_prev(q)) {
        tp = ngx_queue_data(q, my_struct_t, link);
        printf("%d ", tp->num);
    }

    printf("\noffsetof(my_struct_t, link) = %lu\n", offsetof(my_struct_t, link));

    q = ngx_queue_middle(&queue_container);
    tp = ngx_queue_data(q, my_struct_t, link);
    printf("middle: %d\n", tp->num);

 return 0;
}
```
