# NGINX 判断事件过期

在对连接的处理过程中，可能会发生连接的读、写事件过期的问题。来看看 NGINX 是怎么解决的。

这种事件过期现象只会在**事件驱动模块**中发生，在事件消费模块中则不用担心这个问题，由于 NGINX 提供了诸如`select`, `epoll`, `kqueue`等各种事件驱动模块，所以这里以常见的`epoll`为例。

## 事件与连接

对于一个连接，它所对应的读、写事件可能会过期，在讨论过期事件之前，先来看看事件和连接在 NGINX 中是如何对应的。

在`ngx_cycle_s`结构中有以下几个字段：

```c
struct ngx_cycle_s {
    ...
    ngx_connection_t        *connections;
    ngx_event_t             *read_events;
    ngx_event_t             *write_events;
    ...
};
```

从字面意思上可以看出分别是表示连接、读事件和写事件的三个数组。NGINX 认为每一个连接一定至少需要一个读事件和一个写事件，有多少个连接就分配多少个读、写事件。每一个连接和其对应的读、写事件通过共享一个数组下标而被绑定在一起。

那么什么是**过期事件**呢？举个例子，假设`epoll_wait`一次返回 3 个事件，在第一个事件的处理过程中，由于业务的需要而关闭了一个连接，而这个连接恰好对应第三个事件，那么这样的话，在处理到第三个事件时，这个事件就已经是过期事件了，是不能处理的。当然我们可以简单的将这个连接的套接字 fd 设置为 -1 然后再放回连接池中去，但是这样不能解决所有问题：

```c
struct ngx_connection_s {
    ...
    ngx_socket_t            fd;
    ...
};
```

假设第三个事件对应的`ngx_connection_t`连接中的套接字 fd 为 50，处理第一个事件时把这个套接字关闭了，并将其设置为 -1，然后使用`ngx_free_connection`将该连接放回到连接池中。然后在`ngx_epoll_process_events`方法的循环中开始处理第二个事件：

```c
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    ...

    events = epoll_wait(ep, event_list, (int) nevents, timer);

    ...

    for (i = 0; i < events; i++) {
        c = event_list[i].data.ptr;
        ...
    }

    ...
}
```

而第二个事件恰好是一个**建立新连接**事件，所以就得调用`ngx_get_connection`从连接池中取出一个连接，而取出的新连接很有可能就是在处理第一个事件时释放掉的那个连接(即对应第三个事件的那个连接)，而由于套接字 fd 50 刚刚被释放，Linux 内核很可能就把它再分配给了刚刚新建立的连接。

然后在处理第三个事件时，由于事件中仍然持有该连接的指针：

```c
struct ngx_event_s {
    void            *data;
    ...
};
```

`ngx_event_s`中的`data`字段表示事件相关的对象，一般都是指向`ngx_connection_t`连接对象。

但是这个链接其实已经被放回到连接池中，然后再分配给事件 2 了，所以对这个连接而言，事件 3 是过期了的，也就是说事件和连接不匹配了，一旦处理了这个事件那么肯定出错。那么怎么判断这种**不匹配**呢？这就是下面该做的事情了。

## 使用 `instance` 标志解决事件过期问题

NGINX 使用一个`instance`标志位来判断事件是否过期。

在`ngx_event_s`中有以下字段：

```c
struct ngx_event_s {
    ...
    unsigned            instance;
    ...
};
```

由于我们是判断**事件**和**连接**的不一致性，那么既然事件中有一个`instance`标志位，那么很自然地我们也会想往`ngx_connection_t`结构中加一个`instance`标志位。但是 NGINX 并不是这样做的，我们知道连接是从`ngx_cycle_t`中的`connections`连接池中取出来的，是一个`ngx_connection_t`的指针，而**指针的最后一位一定是 0**，而 NGINX 就利用了这个特性，既然最后一位一定是 0，不如用它来表示`instance`标志。

那么怎么利用`instance`标志来判断事件过期呢？解决方法就在使用`ngx_get_connection`方法从连接池中取出连接时对该连接对应的读、写事件的`instance`标志进行取反操作：

```c
ngx_connection_t *
ngx_get_connection(ngx_socket_t s, ngx_log_t *log)
{
    ...

    /*
     * 由于连接和读、写事件共享一个下标
     * 如果把整个连接都 memzero 掉，那么又得从`connections`数组中找到该连接的下标
     * 然后从`read_events`和`write_events`中把对应下标的读、写事件和该连接绑定到一起
     * 浪费时间，所以先把读、写事件给记住
     */
    rev = c->read;
    wev = c->write;

    ngx_memzero(c, sizeof(ngx_connection_t));

    c->read = rev;
    c->write = wev;
    c->fd = s;
    c->log = log;

    instance = rev->instance;

    ngx_memzero(rev, sizeof(ngx_event_t));
    ngx_memzero(wev, sizeof(ngx_event_t));

    // 重点在这里
    rev->instance = !instance;
    wev->instance = !instance;

    // epoll 模块不用理会 ngx_event_t 的 index 字段
    rev->index = NGX_INVALID_INDEX;
    wev->index = NGX_INVALID_INDEX;

    rev->data = c;
    wev->data = c;

    wev->write = 1;

    return c;
}
```

然后在`ngx_epoll_process_events`方法中处理事件时则检测：

```c
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    ...

    events = epoll_wait(ep, event_list, (int) nevents, timer);

    ...

    for (i = 0; i < events; i++) {
        c = event_list[i].data.ptr;

        // 取出该连接的 instance 标志位
        instance = (uintptr_t) c & 1;
        // 获取到连接真正的地址
        c = (ngx_connection_t) ((uintptr_t) c & (uintptr_t) ~1);

        // 检测读事件是否过期
        rev = c->read;
        if (c->fd == -1 || rev->instance != instance) {

            /*
             * the stale event from a file descriptor
             * that was just closed in this iteration
             */

            ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                           "epoll: stale event %p", c);
            continue;
        }

        ...

        revents = event_list[i].events;

        ...

        // 检测写事件是否过期
        wev = c->write;
        if ((revents & EPOLLOUT) && wev->active) {

            if (c->fd == -1 || wev->instance != instance) {

                /*
                 * the stale event from a file descriptor
                 * that was just closed in this iteration
                 */

                ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                               "epoll: stale event %p", c);
                continue;
            }

            ...
        }

        ...
    }

    ...
}
```

## 总结
