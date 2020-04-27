# NGINX 解决惊群问题

Thundering Herd

## 1. `accept` 惊群

## 2. `epoll` 惊群

## 3. NGINX 是如何解决惊群问题的？

既然惊群是多个子进程同一时刻监听同一个端口才产生的问题，那么 Nginx 的解决办法就很直观：它只允许同一时刻只能由一个 worker 进程监听端口，此时新连接事件就只能唤醒为一个正在监听端口的 worker 子进程了，也就不会发生惊群了。

事件的处理工作是在`ngx_event.c/ngx_process_events_and_timers`函数做的。

```c
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    ...

    if (ngx_use_accept_mutex) {
        if (ngx_accept_disabled > 0) {
            ngx_accept_disabled--;

        } else {
            if (ngx_trylock_accept_mutex(cycle) == NGX_ERROR) {
                return;
            }

            if (ngx_accept_mutex_held) {
                flags |= NGX_POST_EVENTS;

            } else {
                if (timer == NGX_TIMER_INFINITE
                    || timer > ngx_accept_mutex_delay)
                {
                    timer = ngx_accept_mutex_delay;
                }
            }
        }
    }

    ...
}
```

解决惊群问题的方法就体现在上面这段代码中。

`ngx_accept_mutex`是 NGINX 各个 worker 进程之间共享的一把锁，谁抢到了这把锁，就可以把监听描述符加入到`epoll`中去，然后在`epoll_wait`时就可以得到新连接到来的通知了。

置于`ngx_accept_disabled`，是和负载均衡有关的，某个 worker 进程处理的连接超过了该阈值，Nginx 不让他参与锁的竞争了(过一段时间才可以)。所以主要看`ngx_trylock_accept_mutex`这个函数调用了。

```c
ngx_int_t
ngx_trylock_accept_mutex(ngx_cycle_t *cycle)
{
    if (ngx_shmtx_trylock(&ngx_accept_mutex)) {

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                       "accept mutex locked");

        if (ngx_accept_mutex_held && ngx_accept_events == 0) {
            return NGX_OK;
        }

        if (ngx_enable_accept_events(cycle) == NGX_ERROR) {
            ngx_shmtx_unlock(&ngx_accept_mutex);
            return NGX_ERROR;
        }

        ngx_accept_events = 0;
        ngx_accept_mutex_held = 1;

        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                   "accept mutex lock failed: %ui", ngx_accept_mutex_held);

    if (ngx_accept_mutex_held) {
        if (ngx_disable_accept_events(cycle, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }

        ngx_accept_mutex_held = 0;
    }

    return NGX_OK;
}
```

函数并不长，首先是调用`ngx_shmtx_trylock`给`ngx_accept_mutex`上锁：

```c
ngx_uint_t
ngx_shmtx_trylock(ngx_shmtx_t *mtx)
{
    return (*mtx->lock == 0 && ngx_atomic_cmp_set(mtx->lock, 0, ngx_pid));
}
```

根据名字也可以看出来是典型的”测试并设置“(TSL)指令的实现，如果锁为 0，说明没有上锁，那么将其值设置为当前进程的 pid 来上锁。`ngx_accept_mutex_held`是子进程的一个全局变量，用来表示该 worker 进程是否持有锁，这个标志主要是让进程内的各个模块了解是否获取到了`ngx_accept_mutex`这把锁。如果`ngx_accept_mutex_held`不为 0，说明上一轮竞争就已经抢到了锁(`ngx_accept_events`在 epoll 模块中不使用)，所以监听套接字已经被加入到了 epoll 中了，就不用再加了，直接返回 OK 就可以了。

**TODO: 有一个问题，如果上一轮已经抢到了锁，那么`ngx_shmtx_trylock`不会失败吗？**

上了锁的进程才能把监听套接字加入到`epoll`中去等待新连接到来的通知，这个工作则是在`ngx_enable_accept_events`中完成的。如果上锁失败，但是`ngx_accept_mutex_held`不为 0，也就是说上一轮抢到了，但是这一轮没有抢到，就需要把监听套接字从它的 epoll 中删除，防止惊群，这个工作是在`ngx_disable_accept_events`中完成的。

### 将监听套接字加入到 epoll 或者从 epoll 中删除

```c
ngx_int_t
ngx_enable_accept_events(ngx_cycle_t *cycle)
{
    ngx_uint_t         i;
    ngx_listening_t   *ls;
    ngx_connection_t  *c;

    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

        c = ls[i].connection;

        if (c == NULL || c->read->active) {
            continue;
        }

        if (ngx_add_event(c->read, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
```

这里遍历所有的监听描述符对应的连接，如果该连接对应的读事件是`active`的，那么说明该连接以及被监听，所以跳过。然后使用`ngx_add_event`加入读事件，为什么是读呢？因为`accept`一个新连接就是读事件。

### 释放锁的时机

我们不能让一个进程长时间地占用锁，不然其他 worker 进程就很难得到处理新连接的机会。
NGINX 采用的方法就是把新连接和普通连接区分开来，放到两个不同的队列中去：

* `ngx_posted_accept_events`: 存放新连接事件
* `ngx_posted_events`: 存放普通读写事件

在`ngx_process_events_and_timers`函数的前一部分，worker 子进程所做的主要工作是抢`ngx_accept_mutex`锁，抢到了之后就把监听描述符加入到 epoll 中去。

然后就需要进入具体的事件驱动模块了，这里只考虑 epoll 模块，这个时候需要调用`ngx_process_events`函数真正对事件进行处理，这个变量其实只是一个宏，它指向的是所选用的事件驱动模块中`actions`结构体中的`process_events`方法，具体到 epoll 模块就是`ngx_epoll_process_events`方法：

```c
static ngx_int_t
ngx_epoll_process_events(ngx_cycle_t *cycle, ngx_msec_t timer, ngx_uint_t flags)
{
    ...

    events = epoll_wait(ep, event_list, (int) nevents, timer);

    ...

    for (i = 0; i < events; i++) {
        c = event_list[i].data.ptr;

        instance = (uintptr_t) c & 1;
        c = (ngx_connection_t *) ((uintptr_t) c & (uintptr_t) ~1);

        rev = c->read;

        if (c->fd == -1 || rev->instance != instance) {
            continue;
        }

        revents = event_list[i].events;

        if (revents & (EPOLLERR|EPOLLHUP)) {

            /*
             * if the error events were returned, add EPOLLIN and EPOLLOUT
             * to handle the events at least in one active handler
             */

            revents |= EPOLLIN|EPOLLOUT;
        }

        if ((revents & EPOLLIN) && rev->active) {

#if (NGX_HAVE_EPOLLRDHUP)
            if (revents & EPOLLRDHUP) {
                rev->pending_eof = 1;
            }

            rev->available = 1;
#endif

            rev->ready = 1;

            if (flags & NGX_POST_EVENTS) {
                queue = rev->accept ? &ngx_posted_accept_events
                                    : &ngx_posted_events;

                ngx_post_event(rev, queue);

            } else {
                rev->handler(rev);
            }
        }

        wev = c->write;

        if ((revents & EPOLLOUT) && wev->active) {

            if (c->fd == -1 || wev->instance != instance) {
                continue;
            }

            wev->ready = 1;

            if (flags & NGX_POST_EVENTS) {
                ngx_post_event(wev, &ngx_posted_events);

            } else {
                wev->handler(wev);
            }
        }
    }

    return NGX_OK;
}
```

代码有点长，就删除了一些注释和一些 log。

* 首先是调用`epoll_wait`得到已经准备好了的事件。
* 然后逐一考察这些事件，首先检查事件是否过期，这个是使用`instance`标志位和`ngx_connection_t`指针的最后一位进行比较来完成的。
* 如果事件是读事件，那么它可能是普通的读事件，也有可能是`accept`事件
* 如果事件是写事件，那么它一定就是普通的事件。

可以看到，在上面这个函数中，`flags`参数中的`NGX_POST_EVENTS`控制着事件的处理方式：

* 如果没有指定这个标志，那么就表示立即处理事件，那么就直接调用事件的`handler`回调方法就可以了
* 如果指定了这个标志位，说明事件需要被延后处理。按照事件是否为新连接事件将它们分别加入到`ngx_posted_accept_events`和`ngx_posted_events`队列中去。

以上就是`ngx_process_events_and_timers`方法的中间流程，还是没有真正处理事件，只是将事件归类了，锁也没有释放。

然后进入该函数的最后一部分：

```c
void
ngx_process_events_and_timers(ngx_cycle_t *cycle)
{
    ...

    ngx_event_process_posted(cycle, &ngx_posted_accept_events);

    if (ngx_accept_mutex_held) {
        ngx_shmtx_unlock(&ngx_accept_mutex);
    }

    ...

    ngx_event_process_posted(cycle, &ngx_posted_events);
}
```

把`delta`相关的部分省略了，这个主要和定时器事件的处理相关，暂时不考虑。

可以看到主要流程分三步：

1. 处理`ngx_posted_accept_events`队列中的事件
2. 释放`ngx_accept_mutex`锁
3. 处理`ngx_posted_events`队列中的事件

这次终于知道锁是什么时候释放的了：就是在处理完新连接事件之后释放的。

```c
void
ngx_event_process_posted(ngx_cycle_t *cycle, ngx_queue_t *posted)
{
    ngx_queue_t  *q;
    ngx_event_t  *ev;

    while (!ngx_queue_empty(posted)) {

        q = ngx_queue_head(posted);
        ev = ngx_queue_data(q, ngx_event_t, queue);

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, cycle->log, 0,
                      "posted event %p", ev);

        ngx_delete_posted_event(ev);

        ev->handler(ev);
    }
}
```

处理队列中事件的方法也很直观，就是逐个调用其`handler`回调即可。

对于监听套接字中的读事件，早在`ngx_event_process_init`方法中就把它们的回调方法根据套接字的类型设置为`ngx_event_accept`或者是`ngx_event_recvmsg`了：

``` C
static ngx_int_t
ngx_event_process_init(ngx_cycle_t *cycle)
{
    ...

    for (i = 0; i < cycle->listening.nelts; i++) {
        ...

        c = ngx_get_connection(ls[i].fd, cycle->log);
        rev = c->read;
        rev->handler = (c->type == SOCK_STREAM) ? ngx_event_accept
                                                : ngx_event_recvmsg;

        ...
    }

    ...
}
```

## 总结

[C++性能榨汁机之惊群问题](http://irootlee.com/juicer_thundering_herd/)

[Linux 惊群详解](https://jin-yang.github.io/post/linux-details-of-thundering-herd.html)

[accept与epoll惊群](https://pureage.info/2015/12/22/thundering-herd.html)

[一个epoll惊群导致的性能问题](https://www.ichenfu.com/2017/05/03/proxy-epoll-thundering-herd/)

[Epoll is fundamentally broken 1/2](https://idea.popcount.org/2017-02-20-epoll-is-fundamentally-broken-12/)

[epoll: add EPOLLEXCLUSIVE flag](https://github.com/torvalds/linux/commit/df0108c5da561c66c333bb46bfe3c1fc65905898)

[什么是惊群，如何有效避免惊群?](https://www.zhihu.com/question/22756773)

[“惊群”，看看nginx是怎么解决它的](https://blog.csdn.net/russell_tao/article/details/7204260)
