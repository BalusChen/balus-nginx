# 监听端口的管理

监听端口是由`listen`配置项来指定的：

```c
struct ngx_command_t ngx_http_core_commands = {
    ...

    { ngx_string("listen"),
      NGX_HTTP_SRV_CONF|NGX_CONF_1MORE,
      ngx_http_core_listen,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    ...
}
```

可以发现，`listen`配置项只能在 server{} 块中出现，所以说它是属于 server 虚拟主机的，而且它与 server{} 块对应的`ngx_http_core_srv_conf_t`结构有着密切的关系。

每个 TCP 监听端口都使用`ngx_http_conf_port_t`结构来保存监听信息：

```c
typedef struct {
    ngx_int_t                  family;
    in_port_t                  port;
    ngx_array_t                addrs;     /* array of ngx_http_conf_addr_t */
} ngx_http_conf_port_t;
```

`family`和`port`这两个字段一看就知道是啥含义，但是`addrs`这个字段有些难以理解，它是一个动态数组，保存的元素是`ngx_http_conf_addr_t`类型。

为什么`ngx_http_conf_port_t`结构中需要`addrs`这个动态数组呢？

```nginx
http {
    server {
        server_name     A;
        listen          127.0.0.1:8000;
        listen          80;
    }

    server {
        server_name     B;
        listenn         80;
        listen          8080;
        listen          173.39.160.51.8000;
    }
}
```

观察上面这个 nginx.conf 配置文件，对于端口 8000，我们既在`A`中监听了`127.0.0.1:8000`，又在`B`中监听了`173.39.160.51:8000`，这个对于具有多 IP 地址的主机来说是很有用的。所有`addrs`数组中是必需的，而其中每个元素都保存着监听着该端口的一个具体的地址。

来看看`ngx_http_conf_addr_t`结构的定义：

```c
typedef struct {
    ngx_http_listen_opt_t      opt;

    ngx_hash_t                 hash;
    ngx_hash_wildcard_t       *wc_head;
    ngx_hash_wildcard_t       *wc_tail;

    /* the default server configuration for this address:port */
    ngx_http_core_srv_conf_t  *default_server;
    ngx_array_t                servers;  /* array of ngx_http_core_srv_conf_t */
} ngx_http_conf_addr_t;
```

* `opt`: 存储着该监听套接字的各种属性
* `hash`, `wc_head`, `wc_tail`这三个哈希表用来加速寻找到对应监听端口上的新连接，确定到底使用哪个 server{} 虚拟主机下的配置来处理它。所以哈希表的值就是`ngx_http_core_srv_conf_t`结构的指针。
* `default_server`: 该监听端口下对应的默认 server{} 虚拟主机
* `servers`: `ngx_http_core_srv_conf_t`结构的动态数组

## 参考
