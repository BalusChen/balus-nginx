# upstream 向下游发送响应体(使用额外缓冲区和临时文件)

在上游网速远比下游网速快的时候，我们需要开启额外的缓冲区并且在必要情况下使用临时
文件来保存上游响应。

## `ngx_event_pipe_t`结构体分析

在使用额外缓冲区和临时文件向下游发送文件的的流程中，`ngx_event_pipe_t`是中心数据
结构。其实从名字也可以看出来，他起的是管道的作用。

```c
typedef struct ngx_event_pipe_s  ngx_event_pipe_t;

struct ngx_event_pipe_s {

};
```

## 源码剖析

使用额外缓冲区和临时文件发送响应的方式也是在`ngx_http_upstream_send_request`函数
中实现的：

```c
static void
ngx_http_upstream_send_response(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ...

    if (u->buffering) {
        ...

        return;
    }

#if (NGX_HTTP_CACHE)

...

#endif

    p = u->pipe;

    p->output_filter = ngx_http_upstream_output_filter;
    p->output_ctx = r;
    ...
    p->temp_file = ngx_palloc(r->pool, sizeof(ngx_temp_filt_t));
    ...
    p->preread_bufs->buf = &u->buffer;
    p->preread_bufs->next = NULL;
    p->preread_size = u->buffer.last = u->buffer.pos;

    u->buffer.last = u->buffer.pos;
    ...
    p->length = -1;
    ...

    p->read_event_handler = ngx_http_upstream_process_upstream;
    p->write_event_handler = ngx_http_upstream_process_downstream;

    ngx_http_upstream_process_upstream(r, u);
}
```

可以看到首先呢是对`u->pipe`这个`ngx_event_pipe_t`类型的字段做了一些的初始化工作。
这里是直接就使用`u->pipe`的，这点需要注意，它是由使用 upstream 机制的 HTTP 模块负
责分配的，而不是在 upstream 中进行分配。

这里还需要注意`preread_bufs`这个字段的设置。它保存的是在接收响应头是额外接收到的响
应体的部分。我们知道响应体和响应头都在`u->buffer`中，当响应头被解析完之后，新接收
到的响应体其实就存在于`[u->buffer.pos, u->buffer.last)`中。当设置好了`preread_bufs`
之后马上就把`u->buffer.last = u->buffer.pos`了，这个会不会有什么负面影响呢？毕竟
直接设置`u->buffer.last`也会影响到`p->preread_bufs->buf->last`。(TODO)

最后设置了一下读事件和写事件到来的回调方法，这一步在使用固定缓冲区的情况下也设置
了，只不过是不同的函数(加了一个`non_buffered`)，然后调用`ngx_http_upstream_process_upstream`
来接收上游响应。

### 读写事件回调

前面在`ngx_http_upstream_send_response`函数中已经看到了读事件和写事件的回调方法分
别被设置为`ngx_http_upstream_process_upstream`和`ngx_http_upstream_process_downstream`
其实这两个函数非常的像：

* `ngx_http_upstream_process_upstream` 方法

```c
static void
ngx_http_upstream_process_upstream(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    if (rev->timedout) {
        ...
    } else {

        if (rev->delayed) {

        }
    }

    if (ngx_event_pipe(p, 0) != NGX_OK) {
        ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
        return;
    }

    ngx_http_upstream_process_request(r, u);
}
```

* `ngx_http_upstream_process_downstream` 方法：

```c
static void
ngx_http_upstream_process_downstream(ngx_http_request_t *r)
{
    if (wev->timedout) {
        ...
    } else {

        if (wev->delayed) {
            ...
        }
    }

    if (nngx_event_pipe(p, 1) != NGX_OK) {
        ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
        return;
    }

    ngx_http_upstream_process_request(r, u);
}
```

可以发现的确是这样，首先都是检查一下读/写事件的超时情况，然后都调用`ngx_event_pipe`
方法，不过读事件调用`ngx_event_pipe`方法时第二个参数为 0，而写事件则使用 1。最后调
用`ngx_http_upstream_process_request`。

可以发现这个套路和使用固定缓冲区的情况是一样的，在使用固定缓冲区的情况中，读事件和
写事件的回调方法也是都调用了`ngx_http_upstream_process_non_buffered_request`，其
第二个参数`do_write`读事件置为 0，写事件则为 1。这里多了一个`ngx_event_pipe`函数，
而且本应该在`ngx_http_upstream_process_request`方法中的`do_write`开关也转移到
`ngx_event_pipe`方法中去了，

### `ngx_event_pipe`方法

那么来具体看一下`ngx_event_pipe`方法

```c
ngx_int_t
ngx_event_pipe(ngx_event_pipe_t *p, ngx_int_t do_write)
{
    ngx_int_t     rc;
    ngx_uint_t    flags;
    ngx_event_t  *rev, *wev;


    for ( ;; ) {
        if (do_write) {

            rc = ngx_event_pipe_write_to_downstream(p);

            if (rc == NGX_ABORT) {
                return NGX_ERROR;
            }

            if (rc == NGX_BUSY) {
                return NGX_OK;
            }
        }

        p->read = 0;
        p->upstream_blocked = 0;

        if (ngx_event_pipe_read_upstream(p) != NGX_OK) {
            return NGX_ABORT;
        }

        if (!p->read && !p->upstream_blocked) {
            break;
        }

        do_write = 1;
    }
```

在一个无限 for 循环中进行数据的收、发工作：

首先如果`do_write`这个开关打开了，说明就要向下游发送信息，那么就调用`ngx_event_pipe_write_to_downstream`
向下游发送数据；然后调用`ngx_event_pipe_read_upstream`尝试着从上游接收数据，如果
成功接收到了数据，说明可以往下游发送了，那么把`do_write`开关打开，下一轮 for 循环
就可以把刚刚接收到的数据发送出去了。

这里在使用`ngx_event_pipe_read_upstream`读取上游数据之前首先把`p->read`和`p->upstream_blocked`
两个标志位设置为 0，待函数返回后再检查，也就是说该函数内部对这两个标志位进行了设置，
所以返回后我们可以通过他们来决定后续流程：

* `p->read == 1` 表示该方法读取到了响应
* `p->upstream_blocked` 表示执行完该方法后需要暂时停止读取上游响应，而并且通过向
下游发送响应来清理出空闲缓冲区。

所以，如果这两个都为 0，说明既没有读取到数据，也不需要向下游发送响应。

### `ngx_event_pipe_read_upstream`读取响应

```c
```

### `ngx_event_pipe_write_to_downstream`发送响应

```c
```

### `ngx_http_upstream_process_request`方法

## 总结

为什么要把“向下游发送响应"和"从上游读取响应"这两件事情放到一个函数中去呢？现在
大概可以明白一点了。由于从上游接收到的数据和向下游发送的数据其实是同一份数据，
甚至使用的是同一块缓冲区来保存，为了节省缓冲区，最好就是接到数据就发送出去，所以
把读和写放到一起：写了数据之后缓冲区变大了，可能就可以读了(说可能是因为也可能上游
没有准备好以及其他各种情况)；读了数据之后缓冲区变小了，但是手头有数据了，所以可能
就可以往下游写了。

### 和使用固定缓冲区的对比
