# nginx 线程池中的通知机制

当线程中的任务完成之后，必须有一种方法通知主线程，从而将控制权交还给主线程。

在 epoll 实现的事件机制中，通知机制是通过`eventfd()`调用来实现的。

## `eventfd` 调用

## 流程

1. 在初始化 epoll 的时候，也初始化了 notify：

```C
static ngx_int_t
ngx_epoll_init(ngx_cycle_t *cycle, ngx_msec_t timer)
{
    ...

#if (NGX_HAVE_EVENTFD)
        if (ngx_epoll_notify_init(cycle->log) != NGX_OK) {
            ngx_epoll_module_ctx.actions.notify = NULL;
        }
#endif

    ...
}

static ngx_int_t
ngx_epoll_notify_init(ngx_log_t *log)
{
    struct epoll_event  ee;

    notify_fd = eventfd(0, 0);
    if (notify_fd == -1) {
        ...
        return NGX_ERROR;
    }

    notify_event.handler = ngx_epoll_notify_handler;
    notify_event.log = log;
    notify_event.active = 1;

    notify_conn.fd = notify_fd;
    notify_conn.read = &notify_event;
    notify_conn.log = log;

    ee.events = EPOLLIN|EPOLLET;
    ee.data.ptr = &notify_conn;

    if (epoll_ctl(ep, EPOLL_CTL_ADD, notify_fd, &ee) == -1) {
        ...
        return NGX_ERROR;
    }
}
```

2. 线程完成任务之后，使用`ngx_notify(ngx_thread_pool_handler);` 通知主线程

```C
static void *
ngx_thread_pool_cycle(void *data)
{
    ngx_thread_pool_t *tp = data;

    for ( ;; ) {
        ...

        task = tp->queue.first;
        tp->queue.first = task->next;

        task->handler(task->ctx, tp->log);

        (void) ngx_notify(ngx_thread_pool_handler);
    }
}
```

3. `ngx_notify`是`ngx_epoll_notify`的别名，在`ngx_epoll_notify`中往`notify_fd`中写入了 1，也就是说计数器增加了 1.

```C
static ngx_int_t
ngx_epoll_notify(ngx_event_handler_pt handler)
{
    static uint64_t inc = 1;

    notify_event.data = handler;

    if ((size_t) write(notify_fd, &inc, sizeof(uint64_t)) != sizeof(uint64_t)) {
        ...
        return NGX_ERROR;
    }

    return NGX_OK;
}
```

这里将传递的 handler 存储在了 read 事件（`notify_event`）的`data`字段中，加上`handler`字段，这里就有两个handler了。这俩需要区别：

1. `handler`字段：
2. `data`字段：

4. 主线程在`ngx_epoll_process_event`监听事件，因为 eventfd 的计数器增加了 1，所以接收到可读事件，此时调用读事件的 handler。

```C
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    events = epoll_wait(ep, event_list, (int) nevents, timer);

    for (i = 0; i < events; i++) {
        c = event_list[i].data.ptr;

        instance = (uintptr_t) c & 1;
        c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

        rev = c->read;

        if (c->fd == -1 || rev->instance != instance) {
            continue;
        }

        revents = event_list[i].events;

        if ((revents & EPOLLIN) && rev->active) {

            rev->ready = 1;
            rev->available = -1;

            rev->handler(rev);
        }

        ...

    return NGX_OK;
}
```

这里需要注意 nginx 是怎么从`epoll_wait`返回的`struct epoll_event`列表中拿到`ngx_event_t`结构的，其实就是利用了`stuct epoll_event`中的`data`字段。

前面在`ngx_epoll_notify_init`时有一句`ee.data.ptr = &notify_conn;`，是将连接作为用户数据保存在 epoll 中，从而在不同函数之间流转。

5. 对于 eventfd，前面注册的可读事件`notify_event`的 handler 为`ngx_epoll_notify_hander`：

```C
static void
ngx_epoll_notify_handler(ngx_event_t *ev)
{
    ssize_t               n;
    uint64_t              count;
    ngx_err_t             err;
    ngx_event_handler_pt  handler;

    if (++ev->index == NGX_MAX_UINT32_VALUE) {
        ev->index = 0;

        n = read(notify_fd, &count, sizeof(uint64_t));

        if ((size_t) n != sizeof(uint64_t){
            ngx_log_error(NGX_LOG_ALERT, ev->log, err,
                          "read() eventfd %d failed", notify_fd);
        }
    }

    handler = ev->data;
    handler(ev);
}
```

由于 notify_fd 是以 ET 边缘触发的方式管理，所以我们实际上不用每次 epoll 感知到可读事件都读 notify_fd，只需要保证计数器到达 UINT64_MAX 之前读取了就可以（读取完计数器就置 0 了）这样还可以减少 read 的开销.

前面调用`ngx_epoll_notfiy`时，已经把用户传入的 handler 放在（文件）全局的的 `notify_event` 的`data`字段了，而且这个`notify_event`作为 eventfd 的读事件，所以现在`handler(ev)`调用的就是用户传入的 handler

6. 现在又回到了线程池中任务完成时向`ngx_notify`传递的`ngx_thread_pool_handler`：

```C
static void
ngx_thread_pool_handler(ngx_event_t *ev)
{
    ngx_event_t        *event;
    ngx_thread_task_t  *task;

    ngx_spinlock(&ngx_thread_pool_done_lock, 1, 2048);

    task = ngx_thread_pool_done.first;
    ngx_thread_pool_done.first = NULL;
    ngx_thread_pool_done.last = &ngx_thread_pool_done.first;

    ngx_memory_barrier();

    ngx_unlock(&ngx_thread_pool_done_lock);

    while (task) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0,
                       "run completion handler for task #%ui", task->id);

        event = &task->event;
        task = task->next;

        event->complete = 1;
        event->active = 0;

        event->handler(event);
    }
}
```

需要注意这里有两个`ngx_event_t`事件，函数参数是 eventfd 的读事件，这个不包含业务逻辑；函数处理现阶段已经完成了的所有 task，每个 task 是由线程池的使用者通过`ngx_thread_task_post`添加到线程池的任务队列的，每个 task 都有一个`event`事件成员：

```C
struct ngx_thread_task_s {
    ngx_thread_task_t   *next;
    ngx_uint_t           id;
    void                *ctx;
    void               (*handler)(void *data, ngx_log_t *log);
    ngx_event_t          event;
};
```

其中 handler 是需要在线程池的线程中完成的操作，而`event`是主线程需要完成的业务逻辑。

比如对于一个 HTTPS 请求，我首先需要进行非对称加密，以校验用户身份，而后进行包体接收、处理等业务操作，但是非对称加密非常耗 CPU，如果许多请求上来，worker 就卡在非对称加解密了，所以我把这个步骤放到线程池中进行，也就是作为`ngx_thread_task_t`的`handler`成员，这部分完成之后需要回到主线程进行业务逻辑，也就是继续处理事件，这个就放到`event`成员中。

## 参考
