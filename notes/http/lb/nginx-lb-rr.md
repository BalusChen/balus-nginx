## nginx 负载均衡

nginx 的一大作用就是作为 load-balancer，在将下游 client 的请求转发到上游服务器时
需要决定使用哪个服务器，或者是为了均衡每台服务器的负载、或是为了保持会话一致性...
等等各种原因，就需要使用到合适的负载均衡算法。

nginx(1.17.6) 内置了 round-roubin、ip-hash 以及 least-conn 这三种负载均衡算法，而
通过使用第三方模块，我们也可以让 nginx 支持诸如 url-hash、least-time、generic-hash
等其他负载均衡算法。

## round-robin 负载均衡

round-robin(简称 rr，即轮询) 是 nginx 中内置的负载均衡算法之一，也是最简单的一种
负载均衡算法。

但是 nginx 中并不是纯粹的 rr，而是 weighted-rr。使用 weighted-rr 时，首先会计算每
台上游服务器的权重，然后选择权重最高的服务器来处理本次请求。

## 源码剖析

### 一个小栗子

在深入源码之前，来看看一个使用 upstream 的例子：

```nginx
http {
    upstream backend {
        server backend1.example.com weight=5 max_conns=64;
        server 127.0.0.1:8080       max_fails=3 fail_timeout=30s;
        server unix:/tmp/backend3;

        server backup1.example.com  backup;
    }
}
```

首先可以看到 nginx 使用`upstream`指令来定义一个一组上游服务器，然后再`upstream{}`
块中使用`server`指令来定义一台上游服务器，而且每台服务器都可以带一些参数来说明这
台服务器的“属性”，具体有哪些可以查看`ngx_http_upstream_module`的文档。

### 相关结构体

在 nginx 的 weighted-rr 算法中，上游服务器是使用`ngx_http_upstream_rr_peer_t`结构
体来表示的：

```c
typedef ngx_http_upstream_rr_peer_t struct ngx_http_upstream_rr_peer_s;

struct ngx_http_upstream_rr_peer_s {
    struct sockaddr             *sockaddr;
    socklen_t                    socklen;
    ngx_str_t                    name;
    ngx_str_t                    server;

    ngx_int_t                    current_weight;
    ngx_int_t                    effective_weight;
    ngx_int_t                    weight;

    ngx_uint_t                   conns;
    ngx_uint_t                   max_conns;

    ngx_uint_t                   fails;
    time_t                       accessed;
    time_t                       checked;

    ngx_uint_t                   max_fails;
    time_t                       fail_timeout;
    ngx_msec_t                   slow_start;
    ngx_msec_t                   start_time;

    ngx_uint_t                   down;
};
```

里面大致分了 5 类：

第一类是和上游服务器地址相关的，这个没啥好说的。

第二类是和权重相关的：

* `current_weight`
* `effective_weight`
* `weight`

第三类是和失败次数相关的：

* `fails`: `fail_timeout`内失败的次数
* `max_fails`: 配置项。`fail_timeout`内允许失败的最大次数，未设置的话则为 0
* `fail_timeout`: 配置项。默认值为 10s

这里需要注意`fail_timeout`这个配置项。与上游服务器通信时可能会失败，当失败次数到
达`max_fails`时，就认为这台服务器不可用了；但是服务器绝大多数情况下不会一直不可用，
而可能是由于网络等原因而暂时不可用；nginx 用`fail_timeout`来表示该段时间内服务器
不可用。

第四类是和慢启动相关的：

* `slow_start`: 防止新添加/恢复的主机被突然增加的请求所压垮，通过这个参数可以让该
主机的weight从0开始慢慢增加到设定值，让其负载有一个缓慢增加的过程。
* `start_time`: 

第五类是与服务器的状态相关的：

* `down`: 用于标识这个服务器永久(permanently)的不可用了
* `accessed`:
* `checked`:

`ngx_http_upstream_rr_peer_t`这个结构体是用来表示一台上游服务器的，但是通常上游
服务器不会只有一台，比如一个`upstream{}`配置块经常会使用`server`来配置好多台服务
器，而且除了主要(primary)服务器外，还有的服务器还会被声明为`backup`，只有当所有
的 primary-server 都不可用才会使用 backup-server，所以这两类 server 也是需要进行
区分的。为此 nginx 还提供了`ngx_http_upstream_rr_peers_t`结构：


```c
typedef struct ngx_http_upstream_rr_peers_s  ngx_http_upstream_rr_peers_t;

struct ngx_http_upstream_rr_peers_s {
    ngx_uint_t                      number;

#if (NGX_HTTP_UPSTREAM_ZONE)
    ngx_slab_pool_t                *shpool;
    ngx_atomic_t                    rwlock;
    ngx_http_upstream_rr_peers_t   *zone_next;
#endif

    ngx_uint_t                      total_weight;

    unsigned                        single:1;
    unsigned                        weighted:1;

    ngx_str_t                      *name;

    ngx_http_upstream_rr_peers_t   *next;

    ngx_http_upstream_rr_peer_t    *peer;
};
```

里面有的字段现在不需要知道是啥，有的则需要：

* `total_weight`:
* `single`:
* `weighted`:
* `next`:
* `peer`:

### weighted-rr 策略的启动

weighted-rr 策略的初始化分为两个步骤：

* 在框架启动时统一进行初始化
* 在客户请求到来时初始化特定的

`ngx_http_upstream_main_conf`里面是对所有`upstream{}`块的配置，而`srv_conf`则是针
对某个特定的`upstream{}`块的配置。

在每个`ngx_http_upstream_srv_conf_t`结构体(表示一个`upstream{}`配置块的配置)中都
有一个`ngx_http_upstream_peer_t`结构体：

```c
typedef ngx_int_t (*ngx_http_upstream_init_pt)(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
typedef ngx_int_t (*ngx_http_upstream_init_peer_pt)(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);

typedef struct {
    ngx_http_upstream_init_pt        init_upstream;
    ngx_http_upstream_init_peer_pt   init;
    void                            *data;
} ngx_http_upstream_peer_t;
```

注意区别`ngx_htp_upstream_rr_peer_t`和这个结构体的区别。这个结构体里面有两个函数
字段`init_upstream`和`init`，后面两个初始化步骤就是通过这两个函数来完成的。

TODO: 如果我想自己写一个负载均衡模块，是不是得通过这个结构体入手？等我看几个第三
方的负载均衡模块然后再来回答这个问题。

#### 1. 框架启动时的初始化

`upstream{}`配置块是放在`http{}`配置块中的，在解析完`http{}`配置块之后，就会调用
所有 http 模块的初始化函数。

对于`ngx_http_upstream_module`来说，调用的就是`ngx_http_upstream_init_main_conf`:

```c
static char *
ngx_http_upstream_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_http_upstream_main_conf_t  *umcf = conf;

    ngx_uint_t                      i;
    ngx_array_t                     headers_in;
    ngx_hash_key_t                 *hk;
    ngx_hash_init_t                 hash;
    ngx_http_upstream_init_pt       init;
    ngx_http_upstream_header_t     *header;
    ngx_http_upstream_srv_conf_t  **uscfp;

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        init = uscfp[i]->peer.init_upstream ? uscfp[i]->peer.init_upstream:
                                            ngx_http_upstream_init_round_robin;

        if (init(cf, uscfp[i]) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    ...
}
```

可以看到，如果`init_upstream`字段为 NULL 的话，就默认使用`ngx_http_upstream_init_round_robin`
来进行初始化(TODO: 所以如果是自己的负载均衡模块，就可以自定义`init_upstream`了)

来看看`ngx_http_upstream_init_round_robin`函数做了些啥：

```c
ngx_int_t
ngx_http_upstream_init_round_robin(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_url_t                      u;
    ngx_uint_t                     i, j, n, w;
    ngx_http_upstream_server_t    *server;
    ngx_http_upstream_rr_peer_t   *peer, **peerp;
    ngx_http_upstream_rr_peers_t  *peers, *backup;

    us->peer.init = ngx_http_upstream_init_round_robin_peer;

    if (us->servers) {
        server = us->servers->elts;

        n = 0;
        w = 0;

        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {
                continue;
            }

            n += server[i].naddrs;
            w += server[i].naddrs * server[i].weight;
        }

        if (n == 0) {
            return NGX_ERROR;
        }

        peers = ngx_palloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t));
        if (peers == NULL) {
            return NGX_ERROR;
        }

        peer = ngx_palloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t) * n);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;
        peers->name = &us->host;

        n = 0;
        peerp = &pers->peer;

        for (i = 0; i < us->servers.nelts; i++) {
            if (server->backup) {
                continue;
            }

            for (j = 0; j < server[i].naddrs; j++) {
                peer[n].sockaddr = server[i].addrs[j].sockaddr;
                ...

                *peerp = &peer[n];
                peerp = &peer[n].next;
                n++;
            }
        }

        us->peer.data = peers;

        /* backup servers */

        n = 0;
        w = 0;

        for (i = 0; i < us->servers->nelts; i++) {
            if (!server[i].backup) {
                continue;
            }

            n += server[i].naddrs;
            w += server[i].naddrs * server[i].weight;
        }

        if (n == 0) {
            return NGX_OK;
        }

        backup = ngx_palloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t));
        if (backup == NULL) {
            return NGX_ERROR;
        }

        peer = ngx_palloc(cf->pool, sizeof(ngx_http_upstream_rr_peer_t) * n);
        if (peer == NULL) {
            return NULL;
        }

        peers->single = 0;
        backup->single = 0;
        backup->number = n;
        backup->weighted = (w != n);
        backup->total_weight = w;
        backup->name = &us->host;

        n = 0;
        peerp = &backup->peer;

        for (i = 0; i < us->servers->nelts; i++) {
            if (!server[i].backup) {
                continue;
            }

            for (j = 0; j < server[i].naddrs; j++) {
                peer[n].sockaddr = server[i].naddrs[j].sockaddr;
                ...

                *peerp = &peer[n];
                peerp = &peer[n].next;
                n++;
            }
        }

        peers->next = backup;

        return NGX_OK;
    }

    /* an upstream implicitly defined by proxy_pass, etc. */
}
```

这段代码实际上可以分为三个部分：

* 统计、初始化 primary server
* 统计、初始化 backup server
* 统计、初始化 implicit server

#### 2. 请求到来时的初始化

上面是只是把一个`upstream{}`中的 server 信息从`srv_conf_t`中归类到了`rr_peers_t`
中去，并且做了一些统计而已。但是其实这点信息是不够的，当选择上游服务器时，我们得
知道哪些服务器已经被选择过了，

所以除了模块的初始化之外，当 client 请求到来时，也会进行初始化：

```c
static void
ngx_http_upstream_init_request(ngx_http_request_t *r)
{
    ...

    if (uscf->peer.init(r, uscf) != NGX_OK) {
        ngx_http_upstream_finalize_request(r, u,
                                           NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    u->peer.start_time = ngx_current_msec;

    if (u->conf->next_upstream_tries
        && u->peer.tries > u->conf->next_upstream_tries)
    {
        u->peer.tries = u->conf->next_upstream_tries;
    }

    ...
}
```

所以`ngx_http_upstream_peer_t`结构中的`init`字段就是用来在 client 请求到来时进行
初始化的。那么这次的初始化具体做了些啥呢：

```c
ngx_int_t
ngx_http_upstream_init_round_robin_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                         n;
    ngx_http_upstream_rr_peer_data_t  *rrp;

    rrp = r->upstream->peer.data;

    if (rrp == NULL) {
        rrp = ngx_palloc(...);
        if (rrp == NULL) {
            return NGX_ERROR;
        }

        r->upstream->peer.data = rrp;
    }

    rrp->peers = us->peer.data;
    rrp->current = NULL;
    rrp->config = 0;

    n = rrp->peers->number;

    // TODO: why 取 primary 和 backup 中的较大者
    if (rrp->peers->next && rrp->peers->next->number > n) {
        n = rrp->peers->next->number;
    }

    if (n <= 8 * sizeof(uintptr_t)) {
        rrp->tried = &rrp->data;
        rrp->data = 0;

    } else {
        // 向上取整
        n = (n + (8 * sizeof(uintptr_t) - 1)) / (8 * sizeof(uintptr_t))
        rrp->tried = ngx_palloc(r->pool, n * sizeof(uintptr_t))
        if (rrp->tried == NULL) {
            return NGX_ERROR;
        }
    }

    r->upstream->peer.get = ngx_http_upstream_get_round_robin_peer;
    r->upstream->peer.free = ngx_http_upstream_free_round_robin_peer;
    r->upstream->peer.tries = ngx_http_upstream_tries(rrp->peers);

    return NGX_OK;
}
```

这个函数做了哪些初始化工作呢？我们知道一个`upstream{}`配置块的配置是使用
`ngx_http_upstream_srv_conf_t`来表示的，每个`srv_conf_t`中都有一个
`ngx_http_upstream_peer_t`类型的`peer`字段，`peer`结构体中有一个`data`字段，这个
字段用来存储框架启动时初始化好了的`ngx_http_upstream_rr_peers_t`结构体。这里首先
为`data`字段分配内存，然后被属于这个`upstream{}`的`peers_t`结构体设置进去后面用。

负载均衡是对于一个`upstream{}`块中的 server 而言的，其他`upstream{}`块中的负载均
衡自然是由其他`upstream{}`自行负责的(虽然走的流程是一样的)。为了记录下这个
`upstream{}`哪些服务器已经被选用过了，这里需要使用一个位图。

我们知道服务器有上游服务器有两种，分别是 primary 和 backup，这两种是需要区别对待
的。

### weighted-rr 策略是如何起作用的

在`ngx_http_upstream_init_round_robin_peer`函数中的最后几步设置了`get`, `free`，
这两个函数就是用来选择和释放上游服务器的。

#### 1. 上游服务器的选取

```c
ngx_int_t
ngx_http_upstream_get_round_robin_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_rr_peer_data_t  *rrp = data;

    ngx_int_t                      rc;
    ngx_uint_t                     i, n;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get rr peer, try: %ui", pc->tries);

    pc->cached = 0;
    pc->connection = NULL;

    peers = rrp->peers;
    ngx_http_upstream_rr_peers_wlock(peers);

    if (peers->single) {
        peer = peers->peer;

        if (peer->down) {
            goto failed;
        }

        if (peer->max_conns && peer->conns >= peer->max_conns) {
            goto failed;
        }

        rrp->current = peer;

    } else {

        peer = ngx_http_upstream_get_peer(rrp);

        if (peer == NULL) {
            goto failed;
        }
    }

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    peer->conns++;

    ngx_http_upstream_rr_peers_unlock(peers);

    return NGX_OK;

failed:

    if (peers->next) {
        rrp->peers = peers->next;

        n = (rrp->peers->number + (8 * sizeof(uintptr_t) - 1))
                / (8 * sizeof(uintptr_t));

        for (i = 0; i < n; i++) {
            rrp->tried[i] = 0;
        }

        ngx_http_upstream_rr_peers_unlock(peers);

        rc = ngx_http_upstream_get_round_robin_peer(pc, rrp);

        if (rc != NGX_BUSY) {
            return rc;
        }

        ngx_http_upstream_rr_peers_wlock(peers);
    }

    ngx_http_upstream_rr_peers_unlock(peers);

    pc->name = peers->name;

    return NGX_BUSY;
}
```

选取上游服务器的工作其实并不完全是在`ngx_http_upstream_get_round_robin_peer`中做
的。

首先他区分了一台上游服务器的情况:

* 如果上游服务器只有一台，那么`rr_peers_t`结构中的`peer`就指向这一台服务器，所以
只需要检查这台服务器是否可用就可以了。
* 如果有多台的话，那么`peer`其实是数组的首地址，所以使用`ngx_http_upstream_get_peer`
函数来真正获取到一台可用服务器。

因为首先是从 primary-server 中寻找最合适的，如果没有找到，那么就会从 backup-server
中去找，而 backup-server 都被放在 primar-server 的`rr_peers_t`结构的`next`字段中。
而其实从 backup-server 中找和从 primary-server 中找的步骤是一样的，所以这里采用了
递归的方法。

##### 上游服务器有多台

```c
static ngx_http_upstream_rr_peer_t *
ngx_http_upstream_get_peer(ngx_http_upstream_rr_peer_data_t *rrp)
{
    time_t                        now;
    uintptr_t                     m;
    ngx_int_t                     total;
    ngx_uint_t                    i, n, p;
    ngx_http_upstream_rr_peer_t  *peer, *best;

    now = ngx_time();

    best = NULL;
    total = 0;

    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {
        n = i / (8 * sizeof(uintptr_t))
        m = (uintptr_t) 1 << i % (8 * sizeof(uintptr_t));

        if (rrp->tried[n] & m) {
            continue;
        }

        if (peer->down) {
            continue;
        }

        if (peer->max_fails
            && peer->fails >= peer->max_fails
            && now - peer->checked <= peer->fail_timeout)
        {
            continue;
        }

        if (peer->max_conns && peer->conns >= peer->max_conns) {
            continue;
        }

        peer->current_weight += peer->effective_weight;
        total += peer->effective_weight;

        if (peper->effective_weight < peer->weight) {
            peer->effective_weight++;
        }

        if (best == NULL || peer->current_weight > best->current_weight) {
            best = peer;
            p = i;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    rrp->current = best;

    n = p / (8 * sizeof(uintptr_t));
    m = (uintptr_t) 1 << p % (8 * sizeof(uintptr_t));

    rrp->tried[n] |= m;

    best->current_weight = total;

    if (now - best->checked > best->fail_timeout) {
        best->checked = now;
    }

    return best;
}
```

#### 2. 上游服务器的释放

```c
```

## 总结

TODO:

1. 正确理解`peer_t`和`peers_t`两个结构体的真正含义
2. 理解 weighted-rr 和 upstream 模块是如何配合的

## 参考

[nginx 中的负载均衡原理](https://juejin.im/entry/585144e861ff4b00683eb92e)

[nginx admin guide: Http Load Balancing](https://docs.nginx.com/nginx/admin-guide/load-balancer/http-load-balancer/)
