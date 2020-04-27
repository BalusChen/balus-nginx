# upstream 向下游发送响应(使用固定缓冲区)

我们已经知道了 Nginx 对上游服务器的响应体的处理方式有 3 种：

* 不转发到下游
* 使用固定缓冲区转发数据到下游
* 使用大量缓冲区和临时文件的方式转发数据到下游

当 nginx 与下游服务器之间的网速更快时，我们采用第二种方法。此时一个固定大小的缓
冲区就足够了。来看看它具体是怎么实现的。

## 使用固定缓冲区转发数据到下游

```c
static void
ngx_http_upstream_send_response(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ...

    if (!u->buffering) {

        if (u->input_filter == NULL) {
            u->input_filter_init = ngx_http_upstream_non_buffered_filter_init;
            u->input_filter = ngx_http_upstream_non_buffered_filter;
            u->iput_filter_ctx = r;
        }

        u->read_event_handler = ngx_http_upstream_process_non_buffered_upstream;
        r->write_event_handler = ngx_http_upstream_process_non_buffered_downstream;
```

如果`u->buffering`标志位没有设置的话，那么说明使用固定缓冲区(即`u->buffer`)来进
行数据的存储转发。这属于前面说过的三种数据转发情况之一:

首先检查`u->input_filter`回调函数是否被设置了。这个回调和`u->create_request`,
`u->process_header`, `u->finalize_request`一样是由使用了 upstream 机制的 HTTP
模块来设置，不过和前面三个回调不同的是，它不是必须设置的。这个回调函数主要是用
来处理响应体的，如果没有设置的话，那么就使用预设的`ngx_http_upstream_non_buffered_filter`
和`ngnx_http_upstream_non_buffered_filter_init`两个方法。

由于我们既要从上游服务器接收响应，又要往下游服务器发送响应，所以这里其实是涉及到
两个方向上的两种不同事件的，而这两个事件都有可能需要被加入到 epoll 或者是加入定时器.
所以我们得事先设置好它们被激活后会调用的回调方法：

* 上游服务器可读。我们把与上游连接的读事件回调方法设置为`ngx_http_upstream_process_non_buffered_upstream`，
这样与上游连接的可读事件发生时就会调用该函数来处理(即从上游服务器读取响应体)。
* 下游服务器可写。我们把与下游连接的写事件回调方法设置为`ngx_http_upstream_process_non_buffered_downstream`，这样与下游连接的可写事件发生时就会调用该函数来处理(即转发包体到下游)。

需要注意，`u->read_event_handler`、`r->write_event_handler`和`input_filter`是不同
的，前两者分别是接收和读取数据的方法，而后者是处理数据的方法。我们得先通过
`u->read_event_handler`读取到数据，然后使用`u->input_filter`对数据进行处理，最后
才能使用`r->write_event_handler`发送数据到下游。

### 已经接收到了部分响应体

我们知道前一阶段我们是在读取并处理上游发来的响应头。在从上游服务器接收数据时，由
于 HTTP 基于 TCP，是流式协议，数据是没有边界的，我们也无法说接收完响应头就暂停，
所以在接收、处理响应头时，我们很有可能已经接收到了一部分响应体。

那么这部分响应体处在`u->buffer`缓冲区的哪一部分呢？响应包头和响应包体都是存在
`u->buffer`中的，而根据`ngx_buf_t`的惯例，`[start, pos)`之间的数据是已经处理完
了的，`[pos, last)`之间的数据是待处理的，`[last, end)`之间的位置是空闲的。所以读
取的部分响应体就存储在`[pos, last)`之间，我们只需要调用`u->input_filter`对其进行
处理即可(而且从这里可以看到`input_filter`不是说一定要接收完所有数据才可以进行处理，
其实前面的`process_header`也是一样，结构体内部一定有字段保存了中间状态)。然后调用
`ngx_http_upstream_process_non_buffered_downstream`把这部分包体向下游发送。

```c
        n = u->buffer.last - u->buffer.pos;

        if (n) {
            u->buffer.last = u->buffer.pos;
            u->state->response_length += n;

            if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
                ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
                return;
            }

            ngx_http_upstream_process_non_buffered_downstream(r);

        } else {
            u->buffer.pos = u->buffer.start;
            u->buffer.last = u->buffer.start;

            if (ngx_http_send_special(r, NGX_HTTP_FLUSH) == NGX_ERROR) {
                ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
                return;
            }

            if (u->peer.connection->read->ready || u->length == 0) {
                ngx_http_upstream_process_non_buffered_upstream(r, u);
            }
        }

        return;
    }   // END if (u->buffering)
```

### 尚未接收到响应体

如果没有接收到相应体(即`u->buffer.pos == u->buffer.last`)，那么由于前面的
(`[start, pos)`)存放的是响应头，而响应头已经发送出去了，所以可以复用这块区域。

然后以`NGX_HTTP_FLUSH`标志位调用`ngx_http_send_special`函数，这个标志位意味着如果
`ngx_http_request_t::out`缓冲区中如果还有待发送的数据，那么就催促着把它们发送出去。

然后检查与上游的连接是否有数据可读(此外还检查了`u->length`是否为 0)，如果有的话，
则调用`ngx_http_upstream_process_non_buffered_upstream`来读取上游响应到`u->buffer`
缓冲区中去。

### 接收和发送响应头的回调方法

在不带缓冲的情况下，接收上游响应和和将响应体发送到下游的回调方法分别是
`ngx_http_upstream_process_non_buffered_upstream`和`ngx_http_upstream_process_non_buffered_down`

#### 1. 接收上游响应

```c
static void
ngx_http_upstream_process_non_buffered_upstream(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ngx_connection_t  *c;

    c = u->peer.connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http upstream process non buffered upstream");

    c->log->action = "reading upstream";

    if (c->read->timedout) {
        ngx_connection_error(c, NGX_ETIMEDOUT, "upstream timed out");
        ngx_http_upstream_finalize_request(r, u, NGX_HTTP_GATEWAY_TIME_OUT);
        return;
    }

    ngx_http_upstream_process_non_buffered_request(r, 0);

```

#### 2. 转发响应到下游

```c
static void
ngx_http_upstream_process_non_buffered_downstream(ngx_http_request_t *r)
{
    ngx_event_t          *wev;
    ngx_connection_t     *c;
    ngx_http_upstream_t  *u;

    c = r->connection;
    u = r->upstream;
    wev = c->write;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http upstream process non buffered downstream");

    c->log->action = "sending to client";

    if (wev->timedout) {
        c->timedout = 1;
        ngx_connection_error(c, NGX_ETIMEDOUT, "client timed out");
        ngx_http_upstream_finalize_request(r, u, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    ngx_http_upstream_process_non_buffered_request(r, 1);
}
```

### 3. `ngx_http_upstream_process_non_buffered_request`方法

##### 1. 用于向下游发送响应的部分

可以发现两个方法很相似，而且都是调用`ngx_http_upstream_process_non_buffered_request`
方法做实际的工作，只不过读取上游响应调用该方法时第二个参数为 0，转发响应到下游调
用该方法时第二个参数为 1.

```c
static void
ngx_http_upstream_process_non_buffered_request(ngx_http_request_t *r,
    ngx_uint_t do_write)
{
    size_t                     size;
    ssize_t                    n;
    ngx_buf_t                 *b;
    ngx_int_t                  rc;
    ngx_connection_t          *downstream, *upstream;
    ngx_http_upstream_t       *u;
    ngx_http_core_loc_conf_t  *clcf;

    u = r->upstream;
    downstream = r->connection;
    upstream = u->peer.connection;

    b = &u->buffer;

    do_write = do_write || u->length == 0;

    if (do_write) {

        if (u->out_bufs || u->busy_bufs || downstream->buffered) {
            rc = ngx_http_output_filter(r, u->out_bufs);

            if (rc == NGX_ERROR) {
                ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
                return;
            }

            ngx_chain_update_chains(r->pool, &u->free_bufs, &u->busy_bufs,
                                    &u->out_bufs, u->output.tag);
        }

        if (u->busy_bufs == NULL) {

            if (u->length == 0
                || (upstream->read->eof && u->length == -1))
            {
                ngx_http_upstream_finalize_request(r, u, 0);
                return;
            }

            if (upstream->read->eof) {
                ngx_log...;

                ngx_http_upstream_finalize_request(r, u, NGX_HTTP_BAD_GATEWAY);
                return;
            }

            if (upstream->read->error) {
                ngx_http_upstream_finalize_request(r, u, NGX_HTTP_BAD_GATEWAY);
                return;
            }

            b->pos = b->start;
            b->last = b->start;
        }
    }
}
```

既然接收上游响应和转发响应到下游都是通过一个函数，那么这个函数里面肯定得有东西来
区别这两种情况，这就是第二个参数`do_write`的作用。

开始可能会以为只有`do_write != 0`才会向下游转发响应，但是其实如果`u->length`这个
字段(这个字段表示还需要接收的上游包体的长度)为 0 的话，就只能往下游转发响应了。

转发响应是检查了`u->out_bufs`, `u->busy_bufs`两个链表和`downstream->buffered`标志
位，为什么要检查这几个字段呢？

* 首先是`u->out_bufs`，这个 chain 链表存储的是来自上游的响应包体。这个可以在函数
`ngx_http_upstream_non_buffered_filter`函数里面看到起作用，这个函数作为默认的
`input_filter`而存在，每次调用它都会把数据加在`u->out_bufs`链表的后面。

* 然后是`u->busy_bufs`，这个和`out_bufs`一样也是一个 chain 链表，为什么要检查这个
链表呢？是因为在`ngx_http_output_filter`中可能没有把`out_bufs`链表中所有的 buf 都
发送完，那些没有被发送的 buf 在`ngx_chain_update_chains`中就会被链接到`busy_bufs`
中去:


* 最后是`u->buffered`标志位。

TODO: `u->buffered`标志位还不懂

只要`if (u->out_bufs || u->busy_bufs || u->buffered)`三个条件有一个满足了，就会调用
`ngx_http_output_filter`这个函数里面调用的是`ngx_http_top_body_filter`，这个函数其实
在`ngx_http_writer_filter_module`模块中被设置为`ngx_http_write_filter`。

TODO: 研究`ngx_http_write_filter`

然后调用`ngx_chain_update_chains`函数更新`out_bufs`, `busy_bufs`和`free_bufs`这
三个链表。在调用`ngx_http_output_filter`向下游发送`out_bufs`指向的响应体时，未必
可以一次性发送完，所以这里做了三件事：
    1. 把`out_bufs`中已经发送完了的`ngx_buf_t`结构体重置(pos 和 last 设置为 start)
       然后添加到`free_bufs`链表中去
    2. 把`out_bufs`中还没有发送完的`ngx_buf_t`结构体添加到`busy_bufs`链表中去。
    3. 清空`out_bufs`链表

```c
void
ngx_chain_update_chains(ngx_pool_t *p, ngx_chain_t **free, ngx_chain_t **busy,
    ngx_chain_t **out, ngx_buf_tag_t tag)
{
    if (*out) {

        if (*busy == NULL) {
            *busy = *out;

        } else {
            for (cl = *busy, cl->next; cl = cl->next) { /* void */ }

            cl->next = *out;
        }

        out = NULL;
    }

    while (*busy) {
        cl = *busy;

        if (ngx_buf_size(cl) != 0) {
            break;
        }

        if (cl->buf->tag != tag) {
            *busy = cl->next;
            ngx_free_chain(cl);
            continue;
        }

        cl->buf->pos = cl->buf->start;
        cl->buf->last = cl->buf->start;

        *busy = cl->next;
        cl->next = *free;
        *free = cl;
    }
}
```

#### 2. 用于从上游接收响应体的部分

```c
    for ( ;; ) {

        if (do_write) {
        ...
        }

        size = b->end - b->last;

        if (size && upstream->read->ready) {
            n = upstream->recv(upstrea, b->last, size);

            if (n == NGX_AGAIN) {
                break;
            }

            if (n > 0) {
                u->state->bytes_received += n;
                u->state->response_length += n;

                if (u->input_filter(u->input_filter_ctx, n) == NGX_ERROR) {
                    ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
                    return;
                }
            }

            do_write = 1;

            continue;
        }

        break;
    }
```

只要缓冲区有可用空间，并且读事件已经准备好了，就可以接收数据。倘若接收到了数据，
那么把`do_write`设置为 1，从而在下一轮循环中可以处理该数据并将其转发到下游。

```c
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (downstream->data == r) {

        if (ngx_handle_write_event(downstream->write, clcf->send_lowat) != NGX_OK) {
            ngx_http_upstream_finalize_request(r, u, NGX_ERROR);
            return;
        }
    }

    if (downstream->write->active && !downstream->write->ready) {
        ngx_add_timer(downstream->write, clcf->send_timeout);

    } else if (downstream->write->timer_set) {
        ngx_del_timer(downstream->write);
    }

    if (upstream->read->active && !upstream->read->ready) {
        ngx_add_timer(upstream->read, clcf->read_timeout);

    } else if (upstream->read->timer_set) {
        ngx_del_timer(upstream->read);
    }
}
```

这里主要注意一下`ngx_event_t`结构体中的`active`以及`ready`这两个标志位。`active`
表示这个事件已经被加入到 epoll 中去了，而`ready`则表示可读/可写。


## 总结

## 参考
