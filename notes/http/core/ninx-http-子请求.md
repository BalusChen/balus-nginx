# nginx http 子请求

现有的模块只能处理一种请求、完成一个任务，而对于需要多个请求协同完成的任务则无能为力。当然，这种任务可以由客户端发起多个请求来完成，但是这样加重了客户端的负担，效率也得不到保证，所以 nginx 提出了子请求机制。

由客户端发起的 HTTP 请求被称为主请求，它直接与客户端通信；而子请求是 nginx 内部发起的特殊 HTTP 请求，由于已经处于 nginx 内部，所以它不需要与客户端建立连接，也不需要解析请求行、请求头等等。

子请求结构上和普通的 HTTP 请求一样，只不过多了从属关系，所以也是用`ngx_http_request_t`表示，其中与子请求有关的几个字段：

```c
struct ngx_http_request_s {
    ...
    
    ngx_http_request_t               *main;
    ngx_http_request_t               *parent;
    ngx_http_postponed_request_t     *postponed;
    ngx_http_post_subrequest_t       *post_subrequest;
    ngx_http_posted_request_t        *posted_requests;
    
    ...
};


typedef struct {
    ngx_http_post_subrequest_pt       handler;
    void                             *data;
} ngx_http_post_subrequest_t;


typedef struct ngx_http_posted_request_s  ngx_http_posted_request_t;

struct ngx_http_posted_request_s {
    ngx_http_request_t               *request;
    ngx_http_posted_request_t        *next;
};
```

* `main`就是指出主请求，`parent`则是指出父请求，这两者并不是等价的，主请求是一个绝对概念，它只有一个，而父请求是一个相对概念，可以有多个。
* `postponed` 
* `post_subrequest`表示的是该子请求结束后的回调方法，以及该回调方法的参数。
* `posted_requests`


## 子请求的创建

子请求通过`ngx_http_subrequest`函数创建，这个函数比较长，分段来看一下：

```c
ngx_int_t
ngx_http_subrequest(ngx_http_request_t *r,
    ngx_str_t *uri, ngx_str_t *args, ngx_http_request_t **psr,
    ngx_http_post_subrequest_t *ps, ngx_uint_t flags)
{
    ngx_time_t                    *tp;
    ngx_connection_t              *c;
    ngx_http_request_t            *sr;
    ngx_http_core_srv_conf_t      *cscf;
    ngx_http_postponed_request_t  *pr, *p;

    if (r->subrequests == 0) {
        ...
        return NGX_ERROR;
    }

    if (r->main->count >= 65535 - 1000) {
        ...
        return NGX_ERROR;
    }

    if (r->subrequest_in_memory) {
        ...
        return NGX_ERROR;
    }
```

首先是函数签名，参数还挺多：

* `r`：父请求
* `uri`：子请求的 uri，用于决定访问哪个 location{}
* `args`：子请求的参数，这个不是必需的
* `psr`：子请求的指针，是一个结果参数
* `ps`：子请求结束时的回调方法以及该回调的参数
* `flags`：用于设置子请求的一些属性

函数首先是判断是否可以创建子请求，nginx 对此有 3 个要求（依次对应 3 个`if`判断）：

* 请求树的层级不得超过 51 层
* 请求树中节点个数不得超过 64535
* `subrequest_in_memory`置位的子请求不得再创建子请求

```c
    sr = ngx_pcalloc(r->pool, sizeof(ngx_http_request_t));
    if (sr == NULL) {
        return NGX_ERROR;
    }

    sr->signature = NGX_HTTP_MODULE;

    c = r->connection;
    sr->connection = c;

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
    sr->main_conf = cscf->ctx->main_conf;
    sr->srv_conf = cscf->ctx->srv_conf;
    sr->loc_conf = cscf->ctx->loc_conf;

    sr->pool = r->pool;

    sr->method = NGX_HTTP_GET;
    sr->request_line = r->request_line;
    sr->uri = *uri;

    if (args) {
        sr->args = *args;
    }

    sr->subrequest_in_memory = (flags & NGX_HTTP_SUBREQUEST_IN_MEMORY) != 0;
    sr->waited = (flags & NGX_HTTP_SUBREQUEST_WAITED) != 0;
    sr->background = (flags & NGX_HTTP_SUBREQUEST_BACKGROUND) != 0;
```

接下来就是设置`ngx_http_request_t`中的一些字段了，其中有几点值得关注：

* 子请求和父请求共享内存池，这就说明，在子请求结束时，其分配的数据在父请求中还是可以访问的
* 子请求默认的 HTTP 方法为 GET，但是我们可以在函数外部对齐进行修改
* 子请求不能跨 server{}，这是因为子请求并不涉及
* 

```c
    sr->main = r->main;
    sr->parent = r;
    sr->post_subrequest = ps;
    sr->read_event_handler = ngx_http_request_empty_handler;
    sr->write_event_handler = ngx_http_handler;

    sr->variables = r->variables;

    sr->log_handler = r->log_handler;

    if (sr->subrequest_in_memory) {
        sr->filter_need_in_memory = 1;
    }

```

为该子请求设置其主请求、父请求以及子请求结束后的回调方法。

默认产生的 subrequest 的 read_event_handler 是 dummy，因为 subrequest 不需要从从 client 读取数据（如果是 upstream 的话，还没有发送请求至 upstream，所以也不用读），write_event_handler 是 ngx_http_handler，在`ngx_http_handler`函数中， 由于子请求都带有`internal`标志位（在下面代码中设置），所以默认从 SERVER_REWRITE 阶段开始执行（这个阶段在将请求的 URI 与 location 匹配之前，修改请求的 URI，即重定向），并将`write_event_handler`重新设置为`ngx_http_core_run_phases`并执行该函数

```c
    if (!sr->background) {

        if (c->data == r && r->postponed == NULL) {
            c->data = sr;
        }

        pr = ngx_palloc(r->pool, sizeof(ngx_http_postponed_request_t));
        if (pr == NULL) {
            return NGX_ERROR;
        }

        pr->request = sr;
        pr->out = NULL;
        pr->next = NULL;

        if (r->postponed) {
            for (p = r->postponed; p->next; p = p->next) { /* void */ }
            p->next = pr;

        } else {
            r->postponed = pr;
        }
    }
```

这一块代码涉及到与其他请求的交互，比较难理解。

具有`background`属性子请求用在缓存中，这里暂且略去不讲。如果不是后台子请求（后台子请求不参与数据的产出），那么会将该子请求挂载在其父请求的`postponed`（这个词的意思是"延期的"）链表末尾。这里需要注意子请求完成的顺序和发起的顺序不一定相同，由于需要完成的任务不同，可能后发起的子请求先完成了，为了正确组织子请求返回的数据，nginx 使用`postponed`链表来组织本级请求发起的所有子请求：

```c
typedef struct ngx_http_postponed_request_s  ngx_http_postponed_request_t;

struct ngx_http_postponed_request_s {
    ngx_http_request_t               *request;
    ngx_chain_t                      *out;
    ngx_http_postponed_request_t     *next;
};
```

可以看到里面有`ngx_http_request_t`，还有一个`ngx_chain_t`，这俩是互斥的，也就是说，一个节点可能是请求节点，也可能是数据节点。将子请求挂载在其父请求的`postponed`链表中表示一种延后处理的思想，此时子请求并不会立即开始执行，而是等待 HTTP 引擎调度。父请求在调用`ngx_http_subrequest`创建子请求后，必须返回`NGX_DONE`告诉 HTTP 框架

上面还有一个关于`c->data`的逻辑，这里需要注意；当前正在执行的请求被称为活跃请求，活跃请求被存储在该请求的连接的`data`字段中，

* 活跃请求的产生的响应数据可以立即发送出去
* 子请求的数据要在父请求之前发送出去
* 子请求之间的响应数据发送顺序为创建的顺序

所以，如果父请求为活跃请求，而此时还没有子请求，那么把新创建的子请求设置为活跃请求。

```c
    sr->internal = 1;
    sr->subrequests = sr->subrequests - 1;
    r->main->count++;

    *psr = sr;

    if (flags & NGX_HTTP_SUBREQUEST_CLONE) {
        ...
    }

    return ngx_http_post_request(sr, NULL);
}
```

最后调用`ngx_http_post_request`函数将新创建的子请求挂载到**主请求**的`posted_requests`链表的末尾，这是子请求能够运行的关键。

## 调度子请求运行

子请求是在`ngx_http_run_posted_request`中

```c
void
ngx_http_run_posted_requests(ngx_connection_t *c)
{
    ngx_http_request_t         *r;
    ngx_http_posted_request_t  *pr;

    for ( ;; ) {

        if (c->destroyed) {
            return;
        }

        r = c->data;
        pr = r->main->posted_requests;

        if (pr == NULL) {
            return;
        }

        r->main->posted_requests = pr->next;

        r = pr->request;

        ngx_http_set_log_request(c->log, r);

        r->write_event_handler(r);
    }
}
```

比较简单，就是将主请求的`posted_requests`链表上的子请求的`write_event_handler`都执行一遍。

## 结束子请求

## 总结

子请求比较难懂，对此我有以下几点理解：

* `posted_requests`只在主请求中有效，它将所有的子请求（以及孙子请求等）都聚集在一个单链表中，是为了便于将子请求调度执行（方便的原因有两个，一个是主请求容易找到，二是单链表结构简单容易遍历）
* `postponed`链表散落在各个请求中，从而形成树状结构（请求树），这是为了有序组织各个请求的响应数据，从而可以将其有序发送给客户端

## 参考

[nginx http 子请求笔记](https://ialloc.org/blog/ngx-notes-http-subrequest/)
