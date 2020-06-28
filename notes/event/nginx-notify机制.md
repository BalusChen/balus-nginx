<!-- vim-markdown-toc GFM -->

* [nginx notify 机制的实现](#nginx-notify-机制的实现)
    * [`eventfd(2)`系统调用](#eventfd2系统调用)
        * [1. 进行`read(2)`操作](#1-进行read2操作)
        * [2. 进行`write(2)`操作](#2-进行write2操作)
        * [3. 进行 polling 操作](#3-进行-polling-操作)
        * [4. 进行`close(2)`关闭操作](#4-进行close2关闭操作)
    * [相比普通文件描述符有什么优势？](#相比普通文件描述符有什么优势)
    * [参考](#参考)

<!-- vim-markdown-toc -->

# nginx notify 机制的实现

在 nginx 线程池的实现中，线程完成了任务之后需要通知主线程，这个通知功能由事件模块来完成。

每类事件模块（epoll、kqeue...）等，都需要实现`ngx_event_actions_t`接口，其中`notify`接口就是通知接口。

```C
typedef struct {
    ngx_str_t              *name;

    void                 *(*create_conf)(ngx_cycle_t *cycle);
    char                 *(*init_conf)(ngx_cycle_t *cycle, void *conf);

    ngx_event_actions_t     actions;
} ngx_event_module_t;


typedef struct {
    ngx_int_t  (*add)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*del)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*enable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*disable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*add_conn)(ngx_connection_t *c);
    ngx_int_t  (*del_conn)(ngx_connection_t *c, ngx_uint_t flags);

    ngx_int_t  (*notify)(ngx_event_handler_pt handler);

    ngx_int_t  (*process_events)(ngx_cycle_t *cycle, ngx_msec_t timer,
                                 ngx_uint_t flags);

    ngx_int_t  (*init)(ngx_cycle_t *cycle, ngx_msec_t timer);
    void       (*done)(ngx_cycle_t *cycle);
} ngx_event_actions_t;
```

既然是背靠`epoll`、`kqueue`等事件机制，那么 notify 功能可以想象，也是通过文件描述符实现的。但是和普通的文件描述符有所不同，epoll 中管理的绝大多数是 socket，即网络事件，它们都由网络触发，epoll 机制被动接受；但是 notify 机制的事件触发则是由开发人员决定何时触发。

比如在线程池中通知主线程任务已完成，则是在任务完成后，主动告诉 epoll，epoll 感知到事件发生，调用对应的方法来处理。所以我们需要的是一种类似于普通本地可读可写的文件描述符的描述符，实际上直接用一个普通可读写的文件描述符似乎也是可以的，但是 OS 给我们提供了专用的机制。

比如在 Linux 上的 eventfd 机制。

## `eventfd(2)`系统调用

`eventfd(2)` 是一个 Linux 上的系统调用，用它可以创建一个专门用于事件通知的文件描述符。既然是**专门**用于事件通知的，那么肯定和其他普通可读写的文件描述符有所不同。

```C
#include <sys/eventfd.h>

int eventfd(unsigned initval, int flags);
```

该系统调用创建一个 eventfd 对象，这个对象包含一个 64-bit 的无符号整数计数器，`initval`就是该计数器的初始值，flags 参数用于控制`eventfd()`的一些行为，在 nginx 的使用中设置为 0 了。

* `EFD_CLOEXEC`：close on exec

* `EFD_NONBLOCK`：非阻塞

* `EFD_SEMAPHORE`：主要影响对 fd 的`read(2)`操作

`eventfd()`调用返回一个引用着 eventfd 对象的文件描述符，我们可以在该文件描述符上进行以下操作：

### 1. 进行`read(2)`操作

我们可以对这个 fd 做`read(2)`操作，如果`read(2)`成功，那么可以读取到一个大小为 8-bit 的整数，而且其字节序为主机字节序，所以我们无论在什么平台上，我们都可以直接使用，而无需考虑大小端，比如下面这样：

```C
static void
read_notify(int notify_fd)
{
    size_t    n;
    uint64_t  counter;

    n = read(notify_fd, &counter, sizeof(uint64_t));

    if ((size_t) n != sizeof(uint64_t)) {
        perror("read notify_fd failed");
        return;
    }

    printf("counter: %llu\n", counter);
}
```

如果传入的缓冲区大小不足 8 个字节，那么`read(2)`出错，并且`errno`被设置为`EINVAL`。

对该 fd 的`read(2)`操作的语义收到两方面的影响：

* eventfd 对象中的计数器是否为 0
* 调用`eventfd(2)`是否传递了`EFD_SEMAPHORE`标志（其实还有`EFD_NONBLOCK`标志，不过这个太常见了）

具体有以下几种情况：

* 没有指定`EFD_SEMAPHORE`标志，并且计数器不为0：那么此时`read(2)`返回该计数器的值，而 eventfd 对象中计数器值重置为 0

* 指定了`EFD_SEMAPHORE`标志，并且计数器不为0：那么此时`read(2)`返回该计数器的值，而 eventfd 对象中计数器值减少 1

* 计数器的值为 0，此时`read(2)`操作阻塞，直到该计数器的值大于0，然后按照上面的两种情况进行；如果设置了`EFD_NONBLOCK`，那么`read(2)`返回`EAGAIN`。

### 2. 进行`write(2)`操作

`write(2)`操作将一个无符号 64-bit 整数写入，这个整数被增加至 eventfd 对象中的计数器上。需要注意的是，eventfd 对象中的计数器的最大值为 `UINT64_MAX - 1`，如果`write(2)`产生的这次加法会导致计数器超过这个值，那么`write(2)`操作会被阻塞，知道对这个 fd 进行了`read(2)`操作使得计数器的值满足了条件，或者因为设置了`EFD_NONBLOCK`而直接返回`EAGAIN`：

```C
static void
write_notify(int notify_fd)
{
    ssize_t   n;
    uint64_t  inc = 1;

    n = write(notify_fd, &inc, sizeof(uint64_t));

    if ((size_t) n != sizeof(uint64_t)) {
        perror("write to notify_fd failed");
        return;
    }
}
```

如果传给`write(2)`的缓冲区大小不足 8-byte，或者写入的值是`UINT64_MAX`，那么`write(2)`返回`EINVAL`。

### 3. 进行 polling 操作

既然是让 epoll 管理，那么最重要的就是让 epoll 感知到这个 fd 可读或者可写，那么什么条件下会可读、可写呢？

* 如果 eventfd 对象中的计数器的值不为 0，那么就是可读的
* 如果计数器至少可以递增 1 （在 fd 层面上的表现就是可以往该 fd 中写 1），那么就是可写的
* TODO: 如果

### 4. 进行`close(2)`关闭操作

如果 eventfd 的 fd 不再需要了，那么就得将其关闭。`eventfd(2)`得到的 fd 在`exec`之后仍旧保持打开，除非设置了`EFD_CLOEXEC`。

## 相比普通文件描述符有什么优势？

我们也可以使用管道（`pipe(2)`）来做 notify，但是`eventfd`相比较于`pipe`其内核负担很小，而且只需要一个 fd。

而且，除了在用户空间做事件通知之外，还可以在内核空间和用户空间之间做事件通知，为二者提供了一座桥梁，再配合使用`epoll`等 I/O multiplexing 机制，则可以一边监听传统的文件描述符，以及内核产生的读事件。

> Applications can use an eventfd file descriptor instead of a pipe in all cases where a pipe is used simply to signal events. The kernel overhead of an eventfd file descriptor is much lower than that of a pipe, and only one file descriptor is required (versus the two required for a pipe).
## 参考

[Worker Pool With EventFd](https://www.yangyang.cloud/blog/2018/11/09/worker-pool-with-eventfd/)
