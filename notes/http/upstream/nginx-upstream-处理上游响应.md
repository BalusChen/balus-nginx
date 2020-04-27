# upstream 响应头的读取和处理

在使用 upstream 机制往上游发送请求之后，上游服务器会进行响应。而我们知道基于 TCP
的响应其实就是有序的数据流，那么按道理其实我们只需要按照接收到的顺序来调用 HTTP
模块来进行处理就可以了。

但是其实没有那么简单。主要的困难在协议长度的不确定和协议内容的解析上面。

* 首先应用层协议的响应包可大可小。小的可以只有 128B，大的可以有 5G，如果等接收完
才进行解析，那么很可能会 OutOfMemory 错误。即使把它放到磁盘中，大量的磁盘你 I/O
也会损耗服务器性能。
* 其次，对响应中的所有内容进行解析其实很多情况下是没有必要的。加入上游服务器是
Memcached，从该服务器上下载图片，我们其实只需要解析 Memcached 协议本身就可以了，
图片则没有解析的必要，我们只需要一边接收，一边把它转发给下游客户端即可。

为了解决上面的问题，应用层协议通常都会把请求和响应分为两部分：包头和包体。包头
在前包体在后。包头相当于把不同的协议包之间的共同部分抽象出来，包之间包头都具有
相同的格式，服务器必须解析包头，而对包体则完全不做格式上的要求，服务器是否解析
它将视业务上的需要而定。

包体和包头存储什么样的信息完全取决于应用层协议，包头中的信息通畅必须包含包体的
长度，这也是应用层协议分为包头和包体的主要原因。很多包头还包含协议版本、请求的
方法类型、数据包的序列号等信息，但是这些都不是 upstream 机制所关心的。

## 源码剖析

upstream 中协议包头的解析是在`ngx_http_upstream_process_header`中完成的，需要注
意的是，协议包头的大小是不确定的，所以这个函数很可能会被多次调用。

```c
static void
ngx_http_upstream_process_header(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ssize_t            n;
    ngx_int_t          rc;
    ngx_connection_t  *c;

    c = u->peer.connection;

    if (c->read->timedout) {
        ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_TIMEOUT);
        return;
    }
```

1. 首先检查读事件是否超时，如果超时了的话，就使用`ngx_http_upstream_next`来根据
其配置决定下一步处理()

这个检查和动作在 upstream 代码里面非常常见，TODO:

```c
    if (!u->request_sent && ngx_http_upstream_test_connect(c) != NGX_OK) {
        ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR)
    }
```

2. 然后检查是否已经发送了请求

其实这一块我挺不懂的，因为我不知道为什么要在这个时候检查是否已经发送过请求头部
了。

```c
    if (u->buffer.start == NULL) {
        u->buffer.start = ngx_palloc(r->pool, u->conf->buffer_size);
        ...
        ngx_list_init(&u->headers_in.headers, r->pool, 8, sizeof(ngx_table_elt_t));
        ...
        ngx_list_init(&u->headers_in.trailers, r->pool, 2， sizeof(ngx_table_elt_t));
    }
```

3. 在真正读取数据之前首先要检查缓冲区是否已经分配过了，比如如果是第一次调用
`ngx_http_upstream_process_header`函数，那么缓冲区就是空，所以需要进行分配，然后
就是初始化`headers_in`结构体里面的两个链表。其中`headers`链表用来存储从接收到的上
游响应头中解析出来的头部，`trailers`链表用来存储(TODO)

```c
    for ( ;; ) {

        n = c->recv(c, u->buffer.last, u->buffer.end - u->buffer.last);

        if (n == NGX_AGAIN) {
            ngx_handle_read_event(c->read, 0);
            return
        }

        if (n == 0) {
            /* 连接关闭 */
        }

        if (n == NGX_ERROR || n == 0) {
            ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_FT_ERROR)
            return
        }

        u->state->bytes_received += n;
        u->buffer.last += n;

        rc = u->process_header(r);

        if (rc == NGX_AGAIN) {

            if (u->buffer.last == u->buffer.end) {
                /* too big header */
                ngx_http_upstream_next(r, u, NGX_HTTP_UPSTREAM_INVALID_HEADER);
                return;
            }

            continue;
        }

        break;
    }
    if (rc == NGX_HTTP_UPSTREAM_INVALID_HEADER) {

    }

    if (rc == NGX_ERROR) {

    }

    /* rc == NGX_OK */
    ...
    if (ngx_http_upstream_process_headers(r, u) != NGX_OK) {
        return;
    }

    ngx_http_upstream_send_response(r, u);
}
```

然后开始接受上游服务器发送回送过来的数据，注意数据被存放在`[u->buffer.last, u->buffer.end)`
之间，如果成功接收到了数据，那么调用 HTTP 模块设置好的`process_header`回调方法来
进行解析。

解析这部分有点迷惑，如果`process_header`回调方法返回`NGX_AGAIN`，说明缓冲区中的
数据还不够，还需要继续进行读取。这可能是因为缓冲区满了，所以得对这种情况进行检
查。如果是因为缓冲区不够了，说明响应头是在太大了。

但是`process_header`还可能返回其他返回值，比如`NGX_OK`, `NGX_HTTP_INVALID_HEADER`，
`NGX_ERROR`, 但是这几个返回值的处理和`NGX_AGAIN`不同，`NGX_AGAIN`是在`for( ;; )`
循环中，但是其他几个却是在循环外面处理的。如果`process_header`的返回值不是`NGX_AGAIN`，
那么就`break`出循环，开始处理其他几种返回值。为什么不是`NGX_AGAIN`就 break 出循环呢？

看一个例子(来自《深入理解 NGINX》):

```c
static ngx_int_t
ngx_http_testupstream_process_header(ngx_http_request_t *r)
{
    for ( ;; ) {
        rc = ngx_http_parse_header(r, r->upstream->buffer, 1);

        if (rc == NGX_OK) {
            /* 成功解析出一行 */
            ...
            continue;
        }

        if (rc == NGX_HEADER_DONE) {
            /* 成功解析出所有的 header */
            ...
            return NGX_OK;
        }

        if (rc == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        return NGX_HTTP_INVALID_HEADER;
    }
}
```

上面这个函数看似没有返回`NGX_ERROR`，其实是可能的，在省略了的代码中，比如内存分配
失败啊，就会返回`NGX_ERROR`。上面只有在解析了所有 header 出来才会返回`NGX_OK`，
如果只解析了一个 header，会一直解析，知道解析完所有 header 或者数据不够解析(`NGX_AGAIN`)。

所以说，除了`NGX_AGAIN`，`process_header`的其他返回值都表示解析失败或者所有 header
都解析出来了，也就不用待在这个`for ( ;; )`循环里面了。

不过要是我来写，我肯定会把`process_header`的所有返回值都放到`for`循环里面处理。

5. 读取完所有响应头(没有读取完的话前面就进行处理了，或者加入 epoll，或者
`ngx_http_upstream_next`)之后，现在来处理这些响应头。

`ngx_http_upstream_process_headers`(是`headers`而不是`header`)方法处理从上游服
务器接收到的响应头中解析出来的 header。 这个方法会把已经解析出来的头部设置到
`ngx_http_request_t`结构体中的`headers_out`链表中，这样在调用`ngx_http_send_header`
方法发送你响应包头给客户端时将会发送这些已经设置了的头部。

```c
static ngx_int_t
ngx_http_upstream_process_headers(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
}
```

6. 响应头解析完了，也处理完了，最后就是把响应包头发送到客户端去。

## 总结

## 参考
