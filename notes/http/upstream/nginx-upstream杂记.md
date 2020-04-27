# 有关 upstream 的一些凌乱知识

## upstream 结构体中一些字段的含义

```c
struct ngx_http_upstream_s {
    ngx_chain_t                     *request_bufs;
    ngx_http_upstream_resolved_t    *resolved;
    
    ngx_buf_t                        buffer;
    unsigned                         buffering:1;
}
```

* request_bufs

这个是用来存储

* resolved

这个字段比较难理解。

* buffer 和 buffering

这两个字段一般是配合使用的。buffer 成员用来存储接收自上游服务器的响应内容，但是
它会被复用，所以它具有多种含义：

    - 在使用 process_header 方法解析上游响应的包头时，buffer 中将会保存完整的包头
    - 当 buffering 位为 1，而且此时 upstream 是在向下游转发上游的包体时，buffer
      没有意义
    - 当 buffering 位为 0 时，buffer 缓冲区会被用于反复接收上游的包体，进而向下游
      转发
    - 当 upstream 并不用于转发上游包体时，buffer 会被用于反复接收上游的包体，但是
      此时 HTTP 模块实现的 input_filter 方法需要关注它

buffering 标志位在向 client 转发上游服务器的包体时才有用。

    - 当 buffering 位为 1 时，表示使用多个缓冲区以及磁盘文件来转发上游的响应包体，
      当 Nginx 与上游间的网速远大于 Nginx 与下游间的网速时，让 Nginx 开启更多的内
      存甚至使用磁盘文件来缓存上游的响应包体，这个举动很有意义，因为这样减轻上游
      服务器的并发压力
    - 当 buffering 位为 0 时，表示只使用上面的这一个 buffer 缓冲区来向下游转发响应
      包体


## upstream 的 3 种处理上游响应的方式

upstream 有 3 中处理上游响应的方式，但是 HTTP 模块怎么告诉 upstream 该使用哪一种方
式来处理呢？

* 当请求的`ngx_http_request_t`结构体中的`subrequest_in_memory`标志位为 1 时，将采
用第一种方式，即 upstream 不转发响应包体到下游，而是由 HTTP 模块实现/注册的 input_filter
方法来处理包体
* 当`subrequest_in_memory`标志位为 0 时，upstream 将会转发包体。
    - 当`ngx_http_upstream_conf_t`配置结构体中的 buffering 标志位为 1 时i，将开启
      更多内存和磁盘文件用于转发上游的包体，这意味着上游的网速更快
    - 当 buffering 标志位为 1 时，将只使用固定大小的缓冲区`ngx_http_upstream_t::buffer`
      来转发包体
      
## 如何来指定上游服务器的地址

首先在 nginx.conf 文件中就可以指定。在`http{}`配置块下面有一个直属配置块`upstream{}`

```Nginx
http {
    upstream backend {
        server backend1.example.com       weight=5;
        server 127.0.0.1:8080             max_fails=3 fail_timeout=30s;
        server unix:/tmp/backend3;

        server backup1.example.com:8080   backup;
        server backup2.example.com:8080   down;
    }

    server {
        location / {
            proxy_pass http://backend;
        }
    }
}
```

除此之外，我们还可以直接使用`ngx_http_upstream_t::resolved`成员来直接设置上游服务
器的地址。

```c
typedef struct {
    ngx_str_t                        host;
    in_port_t                        port;
    ngx_uint_t                       no_port; /* unsigned no_port:1 */

    ngx_uint_t                       naddrs;
    ngx_resolver_addr_t             *addrs;

    struct sockaddr                 *sockaddr;
    socklen_t                        socklen;
    ngx_str_t                        name;

    ngx_resolver_ctx_t              *ctx;
} ngx_http_upstream_resolved_t;
```

里面必须设置的是`naddrs`, `sockaddr`, `socklen`这 3 个字段。

##  如何启动 upstream

`ngx_http_upstream_t`结构体中一共有 10 个 回调方法，但那是必须设置的只有 3 个：

* `create_request`
* `process_header`
* `finalize_request`

我们可以在设置好了这几个(其它的如有需要也可以设置)回调函数之后(一般是在
`ngx_http_xxx_handler`进行设置)，然后执行`ngx_http_upstream_init`方法即可启动 upstream
机制。

```c
static ngx_int_t
ngx_http_mymodule_handler(ngx_http_request_t *r)
{
    ...

    r->upstream->create_request = ngx_http_mymodule_create_request;
    r->upstream->process_header = ngx_http_mymodule_process_header;
    r->upstream->finalize_request = ngx_http_mymodule_finalize_request;

    ...
    
    r->main->count++;
    ngx_http_upstream_init(r);
    return NGX_DONE;
}
```

调用了`ngx_http_upstream_init`方法就启动了 upstream 机制，此时一定要注意需要通过
返回`NGX_DONE`来告诉 HTTP 框架暂停执行请求的下一阶段。

这里还得执行`r->main->count++`，将引用计数加一是告诉 HTTP 框架不要销毁请求。因为
HTTP 框架只有在请求的引用计数为 0 时才会真正销毁请求。这样的话，upstream 机制接下
来才能真正接管请求的处理工作。

## `ngx_http_upstream_t`结构体中的几个回调的作用

`ngx_http_upstream_t`结构体中一个有 10 个回调，其中 3 个是 HTTP 模块必须设置的(
就算函数体为空也必须设置，不然启动 upstream 机制后就会发生空指针调用错误)，其他的
虽然不是必须设置，但是有的时候也会用得上，所以来看看这些回调。

### `create_request`

`create_request`是创建发往上游服务器的请求，这个函数的回调场景最简单，它只会被调用
一次(除非启用了 upstream 的失败重试机制)。来看看它的回调场景：

1. 在 Nginx 主循环`ngx_worker_process_cycle`中会定期调用事件模块检查是否有网络事件
发生。
2. 事件模块在接收到 HTTP 请求后会调用 HTTP 框架来处理。假设接收、解析完 HTTP 请求
头部时发现需要由 mymodule(假设这是我们在这个笔记中使用的一个用到了 upstream 机制的
http 模块)来处理，这个时候会调用`ngx_http_mymodule_handler`来处理。
3. 在`ngx_http_mymodule_handler`中会设置与 upstream 相关的各个字段，然后启动 upstream
机制
4. upstream 首先回去检查文件缓存，如果缓存中已经有了合适的响应包，则会直接返回缓存
(当然必须是在使用了文件代理缓存的情况下)。
5. 回调由`ngx_http_mymodule`已经实现了的 create_request 方法
6. mymodule 通过设置`r->upstream->request_bufs`已经决定好发送什么样的请求到上游服
务器
7.  与上游服务器建立连接

### `reinit_request`

这个方法可能会被调用多次，不过它只有在“第一次试图向上游服务器建立连接时由于各种异
常原因失败了"的情况下才会被调用，而且是否被调用还得根据`ngx_http_upstream::conf`
成员中的策略要求来决定是否再次重连。

### `process_header`

`process_header`是用来解析上游服务器基于 TCP 的响应头部，所以这个函数可能会被调用
多次，它的调用次数和它的返回值有关。如果返回的是`NGX_AGAIN`，那么说明没有接收到完
整的响应头部，所以接下来到达的数据还会被当成响应头部接收，并且调用`process_header`
处理；如果返回的是`NGX_OK`，那就说明接收到了完整的响应头部，那么在此次连接的后续过
程中就不会再调用`process_header`。

来看看`process_header`的回调场景：

1. Nginx 主循环中定期调用事件模块，检测是否有网络事件发生
2. 事件模块接收到了上游服务器发送来的响应之后，会回调 upstream 模块进行处理
3. upstream 模块此时可以从套接字缓冲区中读取到来此上游的 TCP 流
   读取的响应会被放到`r->upstream->buffer`中。这里有一点需要注意，在未解析完响应
头部之前，如果多次接收到字符流，所有接收自上游的响应都会被完整地存放到`r->upstream->buffer`
缓冲区中去。所以，如果在解析上游响应头时 buffer 缓冲区都满了却还没有解析到完整的
响应头时(也就是说 process_header 一直返回`NGX_AGAIN`)，那么请求就会出错
4. 调用 mymodule 的 process_header 方法解析`r->upstream->buffer`缓冲区。
5. 如果 process_header 返回`NGX_AGAIN`，表示还没有解析到完整的响应头部，那么下次
还会继续调用 process_header 读取接收到的上游响应

### `input_filter_init`和`input_filter`

这两个回调都用于处理上游的响应包体，Nginx 有预置实现。

## 总结

## 参考

[nginx 实现动态 resolve 的思路](http://abonege.github.io/2017/12/15/nginx实现动态resolve的思路/)

[nginx 负载均衡](https://qidawu.github.io/2017/05/13/nginx-upstream/)
