# Nginx-http请求的多阶段处理(二)

前面已经了解了所有阶段是如何被初始化的，现在来具体看看各个阶段的执行流程。

在真正处理请求时，也就是`ngx_http_handler`中，Nginx 将`write_event_handler`设为`ngx_http_core_run_phases`，来看看这个函数是做什么的：

```c
void
ngx_http_core_run_phases(ngx_http_request_t *r)
{
    while (ph[r->phase_handler].checker) {

        rc = ph[r->phase_handler].checker(r, &ph[r->phase_handler]);

        if (rc == NGX_OK) {
            return;
        }
    }
}
```

前面已经看到，Nginx 中 HTTP 请求的处理流程一共分为 11 个阶段：

```c
typedef enum {
    NGX_HTTP_POST_READ_PHASE = 0,

    NGX_HTTP_SERVER_REWRITE_PHASE,

    NGX_HTTP_FIND_CONFIG_PHASE,
    NGX_HTTP_REWRITE_PHASE,
    NGX_HTTP_POST_REWRITE_PHASE,

    NGX_HTTP_PREACCESS_PHASE,

    NGX_HTTP_ACCESS_PHASE,
    NGX_HTTP_POST_ACCESS_PHASE,

    NGX_HTTP_PRECONTENT_PHASE,

    NGX_HTTP_CONTENT_PHASE,

    NGX_HTTP_LOG_PHASE
} ngx_http_phases;
```

这里主要看一下每个阶段的 checker 方法。大部分 phase 都共用一个 checker 方法，但是也有一些其他

## `NGX_HTTP_POST_READ_PHASE`阶段

这个 phase 是在接收到完整的 HTTP 头部之后的处理阶段，其 checker 为`ngx_http_core_generic_phase`：

```c
ngx_int_t
ngx_http_core_generic_phase(ngx_http_request_t *r, ngx_http_phase_handler_t *ph)
{
    ngx_int_t  rc;


    rc = ph->handler(r);

    if (rc == NGX_OK) {
        r->phase_handler = ph->next;
        return NGX_AGAIN;
    }

    if (rc == NGX_DECLINED) {
        r->phase_handler++;
        return NGX_AGAIN;
    }

    if (rc == NGX_AGAIN || rc == NGX_DONE) {
        return NGX_OK;
    }

    /* rc == NGX_ERROR || rc == NGX_HTTP_...  */

    ngx_http_finalize_request(r, rc);

    return NGX_OK;
}

```

所有的 checker 都是执行 handler，并根据 handler 的返回值决定下一步的操作，简单总结一下`ngx_http_core_generic_phase`阶段的 checker 的处理逻辑：



| handler 返回值            | checker 处理逻辑               | 备注                                                         |
| ------------------------- | :----------------------------- | ------------------------------------------------------------ |
| `NGX_OK`                  | 执行下一个阶段的第一个 handler |                                                              |
| `NGX_DECLINED`            | 执行下一个 handler             | 不一定属于同一个 phase                                       |
| `NGX_AGAIN` || `NGX_DONE` | 继续停留在本阶段               | 着当前 handler 尚未结束，意味着该 handler 在当前 phase 有机会再次被调用 |
| 其他                      | 结束请求                       |                                                              |



## `NGX_HTTP_SERVER_REWRITE_PHASE`阶段

这个 phase 是在将请求的 uri 和 location 匹配之前，修改请求的 uri，即重定向操作。这个 phase 的 checker 为`ngx_http_core_rewrite_phase`：

```c
ngx_int_t
ngx_http_core_rewrite_phase(ngx_http_request_t *r, ngx_http_phase_handler_t *ph)
{
    ngx_int_t  rc;


    rc = ph->handler(r);

    if (rc == NGX_DECLINED) {
        r->phase_handler++;
        return NGX_AGAIN;
    }

    if (rc == NGX_DONE) {
        return NGX_OK;
    }

    /* NGX_OK, NGX_AGAIN, NGX_ERROR, NGX_HTTP_...  */

    ngx_http_finalize_request(r, rc);

    return NGX_OK;
}

```

| handler 返回值 | checker 处理逻辑       | 备注                                             |
| -------------- | ---------------------- | ------------------------------------------------ |
| `NGX_DECLINED` | 继续执行下一个 handler | 下一个 handler 可能当前 handler 不在同一个 phase |
| `NGX_DONE`     | TODO：                 | 没有理解 checker 返回 NGX_OK 的含义              |
| 其他           | 结束请求               |                                                  |

## `NGX_HTTP_FIND_CONFIG_PHASE`阶段

find config 顾名思义是查找 location 配置块的阶段，这个阶段只能由`ngx_http_core_module`处理，而不允许其他模块添加 handler。其 checker 为`ngx_http_core_find_config_phase`：

这个 phase 的 checker 实际上承担了 handler 的作用，所以逻辑比较复杂，后面会专门写一篇 blog 来解读

| handler 返回值 | checker 处理逻辑 | 备注 |
| -------------- | ---------------- | ---- |
|                |                  |      |
|                |                  |      |
|                |                  |      |
|                |                  |      |

## `NGX_HTTP_REWRITE_PHASE`阶段

这个 pahse 是在找到了 location 之后再次重写 uri，这个 phase 的 checker 和前面的`NGX_HTTP_SERVER_REWRITE_PHASE`一样都是`ngx_http_core_rewrite_phase`

## `NGX_HTTP_POST_REWRITE_PHASE`阶段

这个 phase 用于在 rewrite 重写 URL 之后，防止错误的 nginx.conf 配置导致死循环（比如递归地修改 URI），因此

```c
ngx_int_t
ngx_http_core_post_rewrite_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    ngx_http_core_srv_conf_t  *cscf;

    if (!r->uri_changed) {
        r->phase_handler++;
        return NGX_AGAIN;
    }

    r->uri_changes--;

    if (r->uri_changes == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "rewrite or internal redirection cycle "
                      "while processing \"%V\"", &r->uri);

        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return NGX_OK;
    }

    r->phase_handler = ph->next;

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
    r->loc_conf = cscf->ctx->loc_conf;

    return NGX_AGAIN;
}

```

## `NGX_HTTP_PREACCESS_PHASE`阶段

## `NGX_HTTP_ACCESS_PHASE`阶段

```c

```



## `NGX_HTTP_POST_ACCESS_PHASE`阶段

## `NGX_HTTP_PRECONTENT_PHASE`阶段

precontent 阶段原来是 try_files 阶段，并且只允许 try_files 一个模块的 handler，而不允许自定义添加 handler，现在



## `NGX_HTTP_CONTENT_PHASE`阶段

这个 phase 是开发 HTTP 模块时最常用的一个阶段。该阶段用于真正处理请求的内容并且生成响应，这个 phase 和其他 phase 相比有一些特殊：

* 其他 10 个阶段都是只放在`ngx_http_core_main_conf_t`结构体中，也就是说，它们对任何一个 HTTP 请求都是有效的。但是对于`NGX_HTTP_CONTENT_PHASE`阶段中的 HTTP 模块而言，它们有另外一种需求，就是当请求的 URL 匹配了配置文件中的某个`location{}`块时才生效。所以它可以是和`ngx_http_core_loc_conf_t`结构相关的：

```c
struct ngx_http_core_loc_conf_s {
    ...
    ngx_http_handler_pt            handler;
    ...
};
```

### 如何向`NGX_HTTP_CONTENT_PHASE`阶段中添加 handler (一)

所以当我们想把某个 handler 加入到 NGX_HTTP_CONTENT_PHASE 阶段时，可以这样设置：以 flv 模块为例：

```c
static ngx_command_t  ngx_http_flv_commands[] = {

    { ngx_string("flv"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_flv,
      0,
      0,
      NULL },

      ngx_null_command
};

static char *
ngx_http_flv(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_flv_handler;

    return NGX_CONF_OK;
}
```

这样的话，在检测到了`flv`配置项之后，就会调用`ngx_http_flv`把该`location{}`块的 handler 设置为`ngx_http_flv_handler`。

事实上，为了加快处理速度，HTTP 框架又在`ngx_http_request_t`结构中增加两个一个成员`content_handler`，在`NGX_HTTP_FIND_CONFIG_PHASE`阶段就会把它设置为匹配了请求 URI 的`location{}`块对应的`ngx_http_core_loc_conf_t`结构体中的`handler`成员。

### 如何向`NGX_HTTP_CONTENT_PHASE`阶段中添加 handler (二)

当然`NGX_HTTTP_CONTENT_PHASE`阶段也可以用和其他阶段一样的方法设置 handler：把 handler 加入到`cmcf->phases[NGX_HTTP_XXX_PHASE]`动态数组中去(这也是其他 10 个阶段添加 handler 的唯一方法)，以 realip 模块为例(它并不属于`NGX_HTTP_CONTENT_PHASE`阶段)：

```c
static ngx_http_module_t  ngx_http_realip_module_ctx = {
    ...                                    /* preconfiguration */
    ngx_http_realip_init,                  /* postconfiguration */

    ...                                    /* create main configuration */
    ...                                    /* init main configuration */

    ...                                    /* create server configuration */
    ...                                    /* merge server configuration */

    ...                                    /* create location configuration */
    ...                                    /* merge location configuration */
};


static ngx_int_t
ngx_http_realip_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_realip_handler;

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_realip_handler;

    return NGX_OK;
}
```

可以看到`ngx_http_realip_handler`这个处理方法介入了`NGX_HTTP_POST_READ_PHASE`和`NGX_HTTP_PREACCESS_PHASE`这两个阶段，采用的方法是通过**必定会被调用**的`postconfiguration`方法中向全局的`ngx_http_core_main_conf_t`结构体中的`phases[NGX_HTTP_XXX_PHASE]`动态数组中添加`ngx_http_handler_pt`处理方法。

### 两种添加 handler 方法的不同点

虽然`NGX_HTTP_CONTENT_PHASE`阶段也可以这样添加 handler，但是这两种方法还是有点不同的。

* 由于每个`location{}`都对应着一个独立的`ngx_http_core_loc_conf_t`结构体，这样的话，handler 就不再应用于所有的 HTTP 请求，而是仅仅在用户请求的 URL 匹配了 location时才会被调用。
* `ngx_http_core_loc_conf_t`结构体中仅有一个`handler`指针，这也意味着如果采用这种方法添加 handler，那么每个请求在`NGX_HTTP_CONTENT_PHASE`阶段只能有 1 个处理方法。而另外一种方法则没有这个限制，`NGX_HTTP_CONTENT_PHASE`阶段可以经由任意个 HTTP模块处理。
* 当同时使用了这两种方法设置 handler 方法时，只有通过`clcf->handler`设置的方法才会生效，也就是说这种方法的优先级更高；而使用`cmcf->phases[NGX_HTTP_CONTENT_PHASE]`添加的 handler 则不会生效。
* 采用`clcf->handler`设置处理方法时。如果一个`location{}`配置块中有多个 HTTP 模块都试图使用这种方法设置处理方法，那么后面的会覆盖前面设置的。

### `ngx_http_core_handler_phase`这个 checker 的具体执行流程

由于`NGX_HTTP_CONTENT_PHASE`阶段添加 handler 的方法和其他 10 个阶段有所不同，所
以执行阶段也会有点不同。

```c
ngx_int_t
ngx_http_core_content_phase(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph)
{
    if (r->content_handler) {
        r->write_event_handler = ngx_http_request_empty_handler;
        ngx_http_finalize_request(r, r->content_handler(r));
        return NGX_OK;
    }
```

首先检查`content_handler`是否被设置了，前面已经知道了这就是匹配到了的`location{}`块对应的`clcf->handler`。如果设置了，那么仅仅执行`content_handler`，这也验证了前面说的使用`clcf->handler`方法添加的 handler 比使用`cmcf->phases[NGX_HTTP_CONTENT_PHASE]`添加的 handler 具有更高优先级的说法。

但是有一个问题就是为什么要把`r->write_event_handler`设置为`ngx_http_request_empty_handler`呢？(TODO)

如果没有`content_handler`，那么就开始正常的执行流程了，和其他 10 个阶段的执行惯例(根据`handler`的返回值决定`checker`的执行流程)类似：

```c
    rc = ph->handler(r);

    if (rc != NGX_DECLINED) {
        ngx_http_finalize_request(r, rc);
        return NGX_OK;
    }
```

首先执行 handler，如果它的返回值不为`NGX_DECLINED`，就意味着不再执行该阶段的其他 handler 方法，所以直接`ngx_http_finalize_request`结束请求，并返回`NGX_OK`表示把控制权归还给 epoll。

```c
    /* rc == NGX_DECLINED */

    ph++;

    if (ph->checker) {
        r->phase_handler++;
        return NGX_AGAIN;
    }
```

如果 handler 的返回值为`NGX_DECLINED`，则表示**希望继续执行本阶段的下一个 handler 方法**。但是我们不能保证当前 handler 不是最后一个 handler(也就是说下一个 handler 可能不存在)，所以我们先转到下一个 handler，检查它的 checker 以判断是否到达了`handlers`数组的末尾，为什么可以这样判断呢？这里需要回去看看`cmcf->handlers`数组是如何初始化的：

### 如何判断到达了`cmcf->phase_engine.handlers`数组的尾部

```c
// ngx_http.c
static ngx_int_t
ngx_http_init_phase_handlers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf)
{
    ...
    ph = ngx_pcalloc(cf->pool,
                     n * sizeof(ngx_http_phase_handler_t) + sizeof(void *));
    if (ph == NULL) {
        return NGX_ERROR;
    }
    ...
}
```

可以发现为`cmcf->phase_engine.handlers`数组分配内存时，不仅仅分配了 n 个`ngx_http_phase_handler_t`，还额外分配了一个`void *`。

```c
typedef ngx_int_t (*ngx_http_phase_handler_pt)(ngx_http_request_t *r,
    ngx_http_phase_handler_t *ph);

struct ngx_http_phase_handler_s {
    ngx_http_phase_handler_pt  checker;
    ngx_http_handler_pt        handler;
    ngx_uint_t                 next;
};
```

这个额外的`void *`其实就是用于存储`ngx_http_phase_handler_t`中的 checker 的(指针)，它可以作为`handlers`数组结束的标记。

当然我们其实可以直接额外分配 1 个`ngx_http_phase_handler_t`结构:

```c
    ph = ngx_pcalloc(cf->pool, (n + 1) * sizeof(ngx_http_phase_handler_t));
    if (ph == NULL) {
        return NGX_ERROR;
    }
```

然后仍旧通过检查`checker`是否为`NULL`的方法来判断是否到达了`cmcf->phase_engine.handlers`数组的末尾，但是 Nginx 的解决方法更加节省内存，毕竟我们只用到了`ngx_htttp_phase_handler_t`结构体中的`checker`指针。

TODO: 但是我有一个问题，为什么在这里就需要进行判断了呢？毕竟`NGX_HTTP_CONTENT_PHASE`不是最后一个阶段啊，后面不是还有一个`NGX_HTTP_LOC_PHASE`阶段吗？还有就是，为什么前面的阶段就不用进行判断呢？



## 总结

- checker 的返回值的含义是什么？