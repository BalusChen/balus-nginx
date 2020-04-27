# Nginx 状态机解析请求行

对于 HTTP 协议，

## 超时检查

```c
static void
ngx_http_wait_request_handler(ngx_event_t *rev)
{
    ...
    
    c = rev->data;  /* rev->data 存储的是 ngx_connection_t 结构 */
    
    if (rev->timedout) {
        ngx_http_close_connection(c);
        return;
    }
    
    if (c->close) {
        ngx_http_close_connection(c);
        return;
    }
```

在前面`ngx_http_init_connection`函数的最后面的操作就是将读事件加入到定时器和 epoll 中。
那么读事件的回调函数被执行就有两种情况：

* 由于超时: 超时值是根据`post_accept_timeout`这个配置项来设置的。一旦超时就得关闭
该连接
* 由于有数据到来而通知 epoll，从而调用该读事件的回调函数。

不管该回调函数是怎么样被触发的，都有可能导致连接被关闭(比如超时，比如读取数据失败)，
所以还得检查`c->close`。

## 创建缓冲区用于读取请求行

```c
    hc = c->data;  /* c->data 存储的是 ngx_http_connection_t 结构 */
    cscf = ngx_http_get_module_srv_conf(hc->conf_ctx, ngx_http_core_module)

    size = cscf->client_header_buffer_size;

    b = c->buffer;

    if (b == NULL) {
        b = ngx_create_temp_buf(c->pool, size);
        if (b == NULL) {
            ngx_http_close_connection(c);
            return;
        }

        c->buffer = b;

    } else if (b->start == NULL) {
        b->start = ngx_palloc(c->pool, size);
        if (b->start == NULL) {
            ngx_http_close_connection(c);
            return;
        }

        b->pos = b->start;
        b->last = b->start;
        b->end = b->start + size;
    }
```

在读取请求行之前，首先得为它分配缓冲区，缓冲区的大小由`client_header_buffer_size`决定。
为什么有这个`if/else-if`呢？

* `if`: 在第一次分配之前，`c->buffer`结构为空，所以需要分配`ngx_buf_t`和该 buf 所持有
的内存，这些都在`ngx_create_temp_buf`函数中完成。
* `else-if`: 在本函数(`ngx_http_wait_request_handler`)函数的后面一部分，如果读取数据返回
`NGX_AGAIN`，则需要将读事件再次加入定时器，此时会把`c->buffer`持有的内存释放(pfree)，
但是缓冲区结构本身没有释放，所以才有这个`else-if`情况。

## 读取请求行

```c
    n = c->recv(c, b->last, size);

    if (n == NGX_AGAIN) {

        if (!rev->timer_set) {
            ngx_add_timer(rev, c->listening->post_accept_timeout);
            ngx_reusable_connection(c, 1);
        }

        if (ngx_handler_read_event(rev, 0) != NGX_OK) {
            ngx_close_connection(c);
            return;
        }

        if (ngx_pfree(c->poll, b->start) != NGX_OK) {
            ngx_close_connection(c);
        }

        return;
    }

    if (n == NGX_ERROR) {
        ngx_close_connection(c);
        return;
    }

    if (n == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0,
                    "client closed connection");
        ngx_close_connection(c);
        return;
    }
```

这里首先使用读取函数来读取请求行，然后根据函数返回值判断接下来该做什么：

* 如果返回值为`NGX_AGAIN`，
* 如果返回 0，说明对端关闭了连接。
* 返回值小于 0，说明读取出错，直接关闭连接后返回。

然后开始处理成功读取的情况：

```c
    /* 省去 proxy_protocol 的步骤  */

    c->log->action = "reading client request line";

    ngx_reusable_connection(c, 0);

    c->data = ngx_http_create_request(c);
    if (c->data == NULL) {
        ngx_close_connection(c);
        return;
    }

    rev->handler = ngx_http_process_request_line;
    ngx_http_process_request_line(rev);
```

如果成功读取到了数据，那么首先使用以第二个参数 0 来调用`ngx_reusable_connection`，
这样由于前面已经把`c->reusable`设置为 1 了，那么这次调用的效果就是把该连接从队列
中取下(并且因为参数 reusable 为 0 所以不会再次插入到队列头部)，为什么要这样呢？(TODO)

然后创建`ngx_http_request_t`结构，作为`c->data`(原来这个字段用来存储`ngx_http_connection_t`,
现在把`ngx_http_connecion_t`放到了`ngx_http_request_t`结构中的`http_connection`字段去了)。

然后设置回调函数为`ngx_http_process_request_line`并执行该回调(毕竟已经读到了数据，
可以开始解析了)

## 状态机解析请求行

前面`ngx_http_wait_request_handler`主要是等待请求的到来，当请求到来后，将读事件
的 handler 设置为`ngx_http_process_header_line`，这个函数才是真正的读取、解析请
求行。

### 首先检查是否超时

```c
static void
ngx_http_process_header_line(ngx_event_t *rev)
{
    c = rev->data;  /* rev->data 存储的是 ngx_connection_t */
    r = c->data;   /* c->data 存储的是 ngx_http_request_t */
    
    if (rev->timedout) {
        c->timedout = 1;
        ngx_http_close_request(r, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }
```

### 然后开始真正的读取和处理

```c
    rc = NGX_AGAIN;

    for ( ;; ) {

        if (rc == NGX_AGAIN) {
            n = ngx_http_read_request_header(r);

            if (n == NGX_AGAIN || n == NGX_ERROR) {
                break;
            }
        }

        rc = ngx_http_parse_request_line(r, r->header_in);

        if (rc == NGX_OK) {
            ...
        }

        if (rc != NGX_AGAIN) {
          ...
        }

        /* NGX_AGAIN */

        if (r->header_in->pos == r->header_in->end) {
            ...
        }

    }

    ngx_http_run_posted_requests(r);
```

1. 首先调用`ngx_http_request_handler`来读取数据到`r->header_in`中去，
在`ngx_http_alloc_request`函数中，`ngx_http_request`的`header_in`缓冲区被设置的和
`ngx_connection_t`的`buffer`一样指向同一块内存：

2. 然后对数据(如果读取到了的话)调用`ngx_http_parse_request_line`函数进行解析。这
个函数才是真正的使用状态机的地方。根据之歌函数的返回值，我们知道是解析完了，还是
解析失败，又或者是读取的数据还不够，然后决定继续做什么。

```c
static ngx_http_request_t *
ngx_http_alloc_request(ngx_connection_t *)
{
    ...
    r->header_in = hc->busy ? hc->busy->buf : c->buffer;
    ...
}
```

这一点需要注意，

### 看看`ngx_http_read_request_header`函数

#### 1. 首先检查是否有数据尚未被处理

```c
static ssize_t
ngx_http_read_request_header(ngx_http_request_t *r)
{
    c = r->connection;
    rev = c->read;
    
    n = r->header_in->last - r->header_in->pos;
    
    if (n > 0) {
        return n;
    }
```

由于`ngx_buf_t`结构中的`pos`和`last`一般表示的是待处理的数据的范围，所以如果该
范围内还有数据的话，就不用读取了，直接返回去解析。比如第一次调用
`ngx_http_read_request_header`时，因为在`ngx_http_wait_request_line`时已经调用
过一次`recv`了，所以`r->header_in`(也就是`c->buffer`)中已经有数据了，所以第一次调用
一般就直接返回了。

#### 2. 读取数据到缓冲区

如果发现没有待处理的数据，那么就调用 recv 回调函数读取数据。

```c
    if (rev->ready) {
        n = recv(c, r->header_in->last, r->header_in->end - r->header_in->last);

    } else {
        n = NGX_AGAIN;
    
    }

    if (n == NGX_AGAIN) {
        if (!rev->timer_set) {
            ...
        }

        if (ngx_handler_read_event(rev, 0) != NGX_OK) {
            ...
        }

        return NGX_AGAIN;
    }
    
    if (n == 0) {
        ngx_log_error(NGX_LOG_INFO, c->log, 0
                     "client permaturely closed connection");
    }
    
    if (n == 0 || n == NGX_ERROR) {
        c->error = 1;
        c->log->action = "reading client request headers"

        ngx_http_finalize_request(r, NGX_HTTP_BAD_REQUEST);
        return NGX_ERROR;
    }
    
    r->header_in->last += n;

    return n;
}
```

根据读取的返回值来决定下一步动作：

* 返回`NGX_AGAIN`: 说明暂时没有准备好，这就需要继续把读事件放入定时器(如果现在不在的话)，
并把它加入 epoll
* 返回 0: 说明对端关闭了连接。记录好日志，并结束请求
* 返回`NGX_ERROR`: 说明发生了错误，结束请求

## 总结

在`ngx_http_init_connection`对连接进行初始化之后(主要是设置相关配置项)，然后就把
读事件的 handler 设置为`ngx_http_wait_request_handler`，并将其加入到定时器和 epoll
中去。

当`ngx_http_wait_request_handler`被调用时，可能是由于定时器调用，或者是 epoll 接到
通知而被调用，所以首先得检查是否超时了。然后开始为接收数据分配缓冲区，这也是 nginx
设计优良的地方，**只有在一定必要的时候才分配内存**，这样就让内存得到了最高效的利用。

然后开始读取、解析请求行。每次读取完都检查返回值，通过返回值我们可以知道此次读取
究竟是读取成功了，还是发生了错误，还是暂时没有数据读...如果是暂时没有数据读
(`NGX_AGAIN`)，那么我们就得重新把读事件加入到定时器和 epoll。

## 参考
