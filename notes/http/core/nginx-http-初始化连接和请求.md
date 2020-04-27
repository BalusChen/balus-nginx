# 连接和请求的初始化

对于 connection 和 request，Nginx 有着一套详细的执行流程，这可是 HTTP 模块的重点，需要好好理解。
主要是要理解和替换一个 TCP 连接如何被 Nginx 初始化，如何形成 request，如何被服务。

## 创建各种数据结构

```c
void
ngx_http_init_connection(ngx_connection_t *c)
{
    ngx_http_connection_t *hc = ngx_palloc(c->pool,
        sizeof(ngx_http_connection_t));
    c->data = hc;
```

这里首先创建了一个`ngx_http_connection_t`结构，这个结构和`ngx_connection_t`结构不同：

```c
typedef struct {
    ngx_http_addr_conf_t             *addr_conf;
    ngx_http_conf_ctx_t              *conf_ctx;

    ngx_chain_t                      *busy;
    ngx_int_t                         nbusy;

    ngx_chain_t                      *free;

    unsigned                          ssl:1;
    unsigned                          proxy_protocol:1;
} ngx_http_connection_t;
```

这个结构中并不实际存储与连接相关的信息，而是存储一些与监听地址、配置项有关的信息，
以及各种选项。

然后继续：

```c
    ngx_http_port_t *port = c->listening->servers;
    if (port->naddrs > 1) {
        ...

   } else {
       ...
   }
}
```

`c->listening-servers`存储的是该监听套接字锁监听的端口信息，它是`ngx_http_port_t`类型：

```c
typedef struct {
    void        *addrs;
    ngx_uint_t   naddrs;
} ngx_http_port_t;
```

在 Nginx 中，每个`server{}`块都可以针对不同的本机 IP 来监听同一个端口。
而`servers`存储的就是对应着所有监听这个端口的地址。`ngx_http_port_t`和 C 原生数组
类似，只不过还存储着数组中的元素个数。

在 HTTP 框架中，`servers`中每个元素都表示一个地址，存储着其所属的`server{}`块的配置项。
其类型为`ngx_http_in_addr_t`或者`ngx_http_in6_addr_t`:
 
 ```c
 typedef struct {
    in_addr_t                  addr;
    ngx_http_addr_conf_t       conf;
} ngx_http_in_addr_t;

typedef struct {
    struct in6_addr            addr6;
    ngx_http_addr_conf_t       conf;
} ngx_http_in6_addr_t;
 ```
 
这两个结构分别针对 IPv4 和 IPv6，保存着 IP 地址，以及该地址所在的`server{}`块中
的配置项信息:
 
 ```c
struct ngx_http_addr_conf_s {
    /* the default server configuration for this address:port */
    ngx_http_core_srv_conf_t  *default_server;

    ngx_http_virtual_names_t  *virtual_names;

    unsigned                   ssl:1;
    unsigned                   http2:1;
    unsigned                   proxy_protocol:1;
};
 ```
 
当初始化一个连接时，首先需要确定该连接对应的本地监听地址是哪个，然后找到该监听
地址对应的虚拟主机的配置项，也就是`ngx_http_conf_addr_t`结构中的`default_server`
字段。

## 找到该连接所需要的配置项
 
 怎么找呢？如果监听该端口的地址只有一个，也就是`port->naddrs`
 
 ```c
 if (port->naddrs > 1) {
     if (ngx_connection_local_sockaddr(c, NULL, 0) != NGX_OK) {
         ngx_http_close_connection(c);
         return;
     }
     
     switch (c->local_sockaddr->family) {
     case AF_INET6:
         sin6 = (struct sockaddr_in6 *) c->local_sockaddr;
         addr6 = port->addrs;
         
         for (i = 0; i < port->naddrs - 1; i++) {
             if (ngx_memcpy(&addr6[i].addr6, &sin6->sin6_addr, 16) == 0) {
                 break;
             }
         }

         hc->addr_conf = &addr6[i].conf;
         break;

     default:  /* AF_INET */
         sin = (struct sockaddr_in *) c->local_sockaddr;
         addr = port->addrs;
         
         for (i = 0; i < port->naddrs - 1; i++) {
             if (ngx_memcpy(&addr[i].addr, &sin->sin_addr, 16) == 0) {
                 break;
             }
         }
         
         hc->addr_conf = &addr[i].conf;
     
     }

 } else {
     switch (c->local_sockaddr->family) {
     case AF_INET6:
         addr6 = port->addrs;
         hc->addr_conf = &addr6[i].conf;
         break;

     default:  /* AF_INET */
         addr = port->addrs;
         hc->addr_conf = &addr[i].conf;
         break;
     }
 }
 
 hc->conf_ctx = hc->addr_conf->default_server->ctx;
 ```
 
可以看出首先呢调用`ngx_connection_sockaddr`确定该连接的本地地址，由于我们已经知
道了套接字描述符，所以可以直接调用`getsockname`来通过套接字描述符确定该连接对应
的本端地址。

确定了本端地址之后，我们在该和监听该端口的所有地址一个一个`memcpy`比较，这里 for
循环需要注意：

```c
for (i = 0; i < port->naddrs - 1; i++)
```

这里的终止条件不是`i < port->naddrs`，因为`port->addrs`是经过了排序的：
  1. 最前面的是 explicit binding
  2. 然后是 implicit binding
  3. 最后是通配符地址


## 为下一阶段做准备

此时已经找到了配置项，初始化工作基本完成了，接下来的工作就是为下一阶段做准备了。

```c
    rev = c->read;
    rev->handler = ngx_http_wait_request_handler;
    c->write->handler = ngx_http_empty_handler;

    if (rev->ready) {
        if (ngx_use_accept_mutex) {
            ngx_post_event(rev, &ngx_posted_events);
            return;
        }

        rev->handler;
        return;
    }

    ngx_add_timer(rev, c->listening->post_accept_timeout);
    ngx_reusable_connection(c, 1);

    if (ngx_handler_read_event(rev, 0) != NGX_OK) {
        ngx_http_close_connection(c);
        return;
    }
}
```

可以看到首先是把连接对应的读/写事件的回调方法给重新设置一遍，这里将写事件的 handler 设置
为`ngx_http_empty_handler`，这是一个函数体为空的函数，由于现在实际的请求都没有接到，就不会
有发送数据的请求，所以将其设置为这个空函数。

TODO: 但是为什么不直接将`c->write->handler`设置为`NULL`呢？

然后检查`rev->ready`是否设置了，为什么要进行这个检查呢？因为有可能有的套接字设置了`deferred`选项，
而且内核也支持的话，那么内核仅会在套接字实际收到了数据(也就是`ready`标志位被设置)时才会通知 epoll。
这时候`ngx_http_init_connection`就不仅仅只是连接建立成功了，而且是客户有数据发送过来了，这个时候就需要进行处理。

但是如果使用了`accept_mutex`的话(虽然现在应该大部分都不会使用了)，那么由于不能让
本进程占用锁太长时间(不然别的进程就得等好久)，所以得先把该读事件放到`ngx_post_events`
队列中去；否则的话就直接处理。

如果没有实际数据到来，那么我们就首先将读事件加入到定时器中去以避免超时，然后调用
`ngx_reusable_connection`，为什么要调用这个函数呢？

```c
void
ngx_resuable_connection(ngx_connection_t *c, ngx_uint_t reusalbe)
{
    if (c->reusable) {
        ngx_queue_remove(&c->queue);
        ngx_cycle->reusable_connections_n--;
    }
    
    c->reusble = reusable;
    
    if (reusable) {
        ngn_queue_insert_head(
            (ngx_queue_t *) ngx_cycle->reusable_connections_queue, &c->queue);
        ngx_cycle->reusalbe_connection_n++;
    }
}
```

函数非常简单:

* 如果该连接的`reusable`字段已经设置，那么就把它从可重用队列当中取下
* 如果`reusable`参数不为 0，那么将该连接插入到可重用连接队列的头部

那么可重用是什么意思呢？`ngx_cycle`中有一个字段`reusable_connections_queue`，
`ngx_connection_t`结构中也有一个`queue`字段用来链接，当连接被使用的时候
(比如读/写)，会把该连接插入到队列头部。这样下来，越靠近队列尾部的连接，空闲未被
使用的时间越长。那么当连接池不够用的时候，就会关闭掉队列尾部的一些连接
(`ngx_drain_connection`)，这种做法类似 LRU。

由于调用`ngx_http_init_connection`一般说明是 accpt 了，所以该连接被使用了，
所以将其插入到队列头部。

然后再加入到 epoll，
这样又实际数据到来时就会调用`rev->handler`，也就是`ngx_http_wait_request_handler`。

## 总结

`ngx_http_init_connection`函数初始化一条连接，具体做了这些事情：

* 将该连接监听地址有关信息(地址、配置项)以`ngx_http_connection_t`的形式放入连接
(`ngx_connection_t`)中。
* 

## 参考
