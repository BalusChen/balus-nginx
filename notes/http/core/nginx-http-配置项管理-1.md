# nginx HTTP 配置项管理（一）

## HTTP 配置项的一些特点

HTTP 配置项由放在`http{...}`配置块中，其下可以有多个`server{...}`配置块，而每个
`server{...}`配置块下面又可以有多个`location{...}`块，而`location{...}`下面还可以继续
有`location{...}`块，也就是说`location{...}`是可以嵌套的。

就 HTTP 框架而言，我们只需要关心`http{...}`、`server{...}`和`location{...}`这三个配置
块即可，其他诸如`if`等配置块则不属于 HTTP 框架。

nginx 中每个配置项都有自己可以出现的区域，这个是通过`ngx_command_t`中的`type`字段来控制
的，比如下面的`flv_buffer_size`这个配置项它可以同时出现在`http{...}`、`server{...}`和
`location{...}`配置块下，但是需要注意的是，这个配置项其实最后生效的位置只有`location{...}`
块，那么为什么不让它只能出现在`location{...}`块中呢？这个其实是考虑到了配置项的继承，比如
说一个`server{...}`下配置了一个`flv_buffer_size`配置项，那么其下的的有`location{...}`
都可以不进行配置，而直接继承其所在的`server{...}`块中的值就可以了。

```c
static ngx_command_t  ngx_http_flv_commands[] = {

    { ngx_string("flv"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_flv,
      0,
      0,
      NULL },

    { ngx_string("flv_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_flv_conf_t, buffer_size),
      NULL },

      ngx_null_command
};

struct ngx_command_s {
    ngx_str_t             name;
    ngx_uint_t            type;
    char               *(*set)(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
    ngx_uint_t            conf;
    ngx_uint_t            offset;
    void                 *post;
};
```

并不是所有最终在`location{...}`块中生效的值都可以在`http{...}`、`server{...}`和`location{...}`
中使用，这要看它是否有意义，比如说`flv`这个指令，将其直接放在`http{...}`块下，这个时候连
端口都不知道，来了一个`curl http://laputa.world/balus.flv`这样的请求，该`flv`的指令也无法
处理，而只有到`location{...}`块下，比如到了`location ~\.flv { flv; }`这个配置块，才能
真正的处理请求。

## `ngx_http_conf_ctx_t`结构

在`ngx_cycle_t::conf_ctx`数组中存储着**所有核心模块**的配置项结构体，对于`ngx_http_module`，
其使用的配置项结构体是`ngx_http_conf_ctx_t`，这个结构体会经常使用到：

```c
typedef struct {
    void        **main_conf;
    void        **srv_conf;
    void        **loc_conf;
} ngx_http_conf_ctx_t;
```

里面有三个指针数组，这要联系到`ngx_http_module_t`中的三个方法：

```c
typedef struct {
    ngx_int_t   (*preconfiguration)(ngx_conf_t *cf);
    ngx_int_t   (*postconfiguration)(ngx_conf_t *cf);

    void       *(*create_main_conf)(ngx_conf_t *cf);
    char       *(*init_main_conf)(ngx_conf_t *cf, void *conf);

    void       *(*create_srv_conf)(ngx_conf_t *cf);
    char       *(*merge_srv_conf)(ngx_conf_t *cf, void *prev, void *conf);

    void       *(*create_loc_conf)(ngx_conf_t *cf);
    char       *(*merge_loc_conf)(ngx_conf_t *cf, void *prev, void *conf);
} ngx_http_module_t;
```

`main_conf`、`srv_conf`、`loc_conf`分别存放的是所有 HTTP 模块的`create_main_conf`、
`create_srv_conf`、`create_loc_conf`创建的结构体指针。

## 管理直属`http{...}`块的配置项

解析`http{...}`配置块是`ngx_http_block`这个函数负责做的，这个配置指令属于
`ngx_http_module`：

```c
static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    if (*(ngx_http_conf_ctx_t **) conf) {
        return "is duplicate";
    }

    /* the main http context */
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    *(ngx_http_conf_ctx_t **) conf = ctx;

    for (m = 0; cf->cycle->modules[m]; m++) {
        if (cf->cycle->modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = cf->cycle->modules[m]->ctx;
        mi = cf->cycle->modules[m]->ctx_index;
        if (module->create_main_conf) {
            ctx->main_conf[mi] = module->create_main_conf(cf);
        }
        if (module->create_srv_conf) {
            ctx->srv_conf[mi] = module->create_srv_conf(cf);
        }
        if (module->create_loc_conf) {
            ctx->loc_conf[mi] = module->create_loc_conf(cf);
        }
    }
    ...
}
```

在解析`http{...}`时，首先分配一个`ngx_http_conf_ctx_t`，放在`ngx_cycle_t::conf_ctx`
数组的相应位置上。然后调用所有 HTTP 模块的`create_main_conf`、`create_srv_conf`和
`create_loc_conf`回调，将产生的结构体指针放到`ngx_http_conf_ctx_t`的对应数组的对应位置
中去。

为什么要在解析直属`http{...}`块的配置项时调用`create_srv_conf`和`create_loc_conf`呢？
这个前面已经说了，一些指令的最小生效粒度虽然是`location{...}`（这样的配置项一般放在模块
`ngx_http_xxx_loc_conf_t`结构体中，由`create_loc_conf`来创建），但是却可以同时出现在
`http{...}`、`server{...}`和`location{...}`（`server{...}`同理），所以这里需要调用
`create_srv_conf`和`create_loc_conf`来为这种配置项分配内存来存储。

![nginx-http-directive-management-http-block](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/directives/nginx-http-directive-management-http-block.png)

## 管理直属`server{...}`块的配置项

负责解析`server{...}`配置块的是`ngx_http_core_server`函数，这是`ngx_http_core_module`
的一个指令。

```c
static char *
ngx_http_core_server(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    http_ctx = cf->ctx;
    ctx->main_conf = http_ctx->main_conf;

    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = cf->cycle->modules[i]->ctx;
        if (module->create_srv_conf) {
            mconf = module->create_srv_conf(cf);
            ctx->srv_conf[cf->cycle->modules[i]->ctx_index] = mconf;
        }

        if (module->create_loc_conf) {
            mconf = module->create_loc_conf(cf);
            ctx->loc_conf[cf->cycle->modules[i]->ctx_index] = mconf;
        }
    }
    ...
}
```

在解析`server{...}`块时，和前面一样，也是首先创建一个`ngx_http_conf_ctx_t`，然后调用所
有 HTTP 模块的`create_srv_conf`和`create_loc_conf`，需要注意的是这里没有调用`create_main_conf`，
因为最小生效粒度为`http{...}`的配置项绝不应该在`server{...}`块（以及`location{...}`）中
出现。

那么`ngx_http_conf_ctx_t::main_conf`怎么处理呢？这里需要将其指向`http{...}`块下的
`ngx_http_conf_ctx_t::main_conf`，为什么要这么做呢？(TODO)

### `http{...}`是如何和`server{...}`关联的

一个`http{...}`块下可以有多个`server{...}`块，那么他们是怎么关联的呢？这就需要用到
`ngx_http_core_module`了，前面说过解析`http{...}`调用了所有 HTTP 模块的`create_main_conf`
回调，当然也就调用了`ngx_http_core_module`的了，它分配的结构体`ngx_http_core_main_conf_t`
中有一个`servers`字段：

```c
typedef struct {
    ngx_array_t                servers;         /* ngx_http_core_srv_conf_t */
    ...
} ngx_http_core_main_conf_t;
```

而在解析`server{...}`时，会调用所有 HTTP 模块的`create_srv_conf`回调，对于
`ngx_http_core_module`，其分配的是`ngx_http_core_srv_conf_t`结构，一个这样的结构就表
示一个`server{...}`块，所有`server{...}`块的`ngx_http_core_conf_t`都存放在`servers`
数组中

```c
static char *
ngx_http_core_server(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    /* create srv/loc conf */
    ...

    /* the server configuration context */
    cscf = ctx->srv_conf[ngx_http_core_module.ctx_index];
    cscf->ctx = ctx;

    cmcf = ctx->main_conf[ngx_http_core_module.ctx_index];
    cscfp = ngx_array_push(&cmcf->servers);
    *cscfp = cscf;

    /* parse inside the server block */
    ...
}
```

而`ngx_http_core_srv_conf_t`中又有一个`ctx`指针，指向了该`server{...}`块的
`ngx_http_conf_ctx_t`结构体：

```c
typedef struct {
    /* server ctx */
    ngx_http_conf_ctx_t        *ctx;
    ...
} ngx_http_core_srv_conf_t;
```

所以通过`ngx_http_core_main_conf_t`中的`servers`字段，我们可以拿到所有的`server{...}`
块的`ngx_http_core_srv_conf_t`，然后通过其中的`ctx`字段，可以拿到该`server{....}`的
`ngx_http_conf_ctx_t`，其中可以拿到所有 HTTP 模块在该`server{...}`的所有配置项。

![nginx-http-directive-management-server-block](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/directives/nginx-http-directive-management-server-block.png)

## 管理直属`location{...}`块的配置项

负责解析`location{...}`配置块的是`ngx_http_core_location`函数，这是`ngx_http_core_module`
的一个指令。

```c
static char *
ngx_http_core_location(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = pctx->srv_conf;

    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = cf->cycle->modules[i]->ctx;

        if (module->create_loc_conf) {
            ctx->loc_conf[cf->cycle->modules[i]->ctx_index] =
                                                   module->create_loc_conf(cf);
        }
    }

    clcf = ctx->loc_conf[ngx_http_core_module.ctx_index];
    clcf->loc_conf = ctx->loc_conf;
    ...
}
```

整体流程和`ngx_http_core_server`是一样的，都是分配一个`ngx_http_conf_ctx_t`结构体，然后
`main_conf`和`srv_conf`分别指向上一级（可能是`server{...}`也可能是`location{...}`）的
`main_conf`和`srv_conf`。然后对每个 HTTP 模块，调用其`create_loc_conf`回调分配内存，并
将结构存入`loc_conf`数组的对应位置。

有了前面的经验应该想得到这里的`ngx_http_core_loc_conf_t`也有特殊作用，的确是这样的。和前面
的`ngx_http_core_srv_conf_t`一样，`ngx_http_core_loc_conf_t`也用来表示一个`location{...}`
块。而一个`server{...}`块下面可以有多个`location{...}`块，并且`location{...}`还可以嵌
套，所以得用一种数据结构将其串联起来。

在`ngx_http_core_loc_conf`中有以下几个字段：

```c
struct ngx_http_core_loc_conf_s {
    ngx_str_t     name;          /* location name */
    /* pointer to the modules' loc_conf */
    void        **loc_conf;
    ngx_queue_t  *locations;
};
```

其中`loc_conf`指向该`location{...}`的`ngx_http_conf_ctx_t`的`loc_conf`。
QUESTION: 为什么不像`ngx_http_core_srv_conf_t`一样直接用一个`ctx`字段指向整个`server{...}`
块的`ngx_http_conf_ctx_t`呢？
 
`locations`就是用来串联各个`location{...}`的字段，虽然是一个`ngx_queue_t`的指针，但是
实际上分配的是`ngx_http_location_queue_t`：

（TODO: 为什么不直接用 ngx_http_location_queue_t？）

```c
typedef struct {
    ngx_queue_t                      queue;
    ngx_http_core_loc_conf_t        *exact;
    ngx_http_core_loc_conf_t        *inclusive;
    ngx_str_t                       *name;
    u_char                          *file_name;
    ngx_uint_t                       line;
    ngx_queue_t                      list;
} ngx_http_location_queue_t;
```

`queue`字段是真正起链接作用的，其中`exact`和`inclusive`两个指针指向的是这个节点对应的
`ngx_http_core_loc_conf_t`，如果`location`后面的表达式是完全匹配的话，那么就由`exact`
指向，否则由`inclusive`指向。

比如对于下面这个 nginx.conf 配置文件：

```nginx
http {
    server {
        listen          9877;
        servername      laputa;

        location /L1 {
            ...
        }

        location /L2 {
            location /L2/T1 {
                ...
            }
        }
    }

    server {
        listen          8888;
        server_name     world;
    }
}
```

名为`laputa`的`server{...}`块下有 3 个`location{...}`，且存在嵌套现象，最后得到的结构
图如下：

![nginx-http-directive-management-location-block](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/directives/nginx-http-directive-management-location-block.png)

注意这里直属`laputa`这个`server`的`location{...}`只有两个，但是却有三个
`ngx_http_location_queue_t`节点，这是因为这是一个双向循环队列，所以需要一个 dummy head
表示队列是否为空。

具体的链接操作也是在`ngx_http_core_location`函数中做的：

```c
static char *
ngx_http_core_location(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    /* create loc conf */
    ...

    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = pctx->srv_conf;

    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    if (ctx->loc_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /* parse location name */
    ...

    pclcf = pctx->loc_conf[ngx_http_core_module.ctx_index];

    /* check location name(compared with parent location) */
    ...
    
    if (ngx_http_add_location(cf, &pclcf->locations, clcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    /* parse inside the location{...} block */

    save = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_HTTP_LOC_CONF;
    rv = ngx_conf_parse(cf, NULL);
    *cf = save;

    return rv;
}
```

流程比较简单：

1. 调用所有 HTTP 模块的`create_loc_conf`回调
2. 解析紧跟在`location`指令后面的表达式，其中可能存在正则。
3. 和上一级`location`的名字进行比较，内部的`location`名字和外部的`location`名字需要遵守
一定的规则（比如外部没有用正则，内部也不允许用...）
4. 将现在正在解析的`location{...}`链接至上一级（`location`或者`server`）
5. 解析`location{...}`内部的指令

```c
ngx_int_t
ngx_http_add_location(ngx_conf_t *cf, ngx_queue_t **locations,
    ngx_http_core_loc_conf_t *clcf)
{
    ngx_http_location_queue_t  *lq;

    if (*locations == NULL) {
        *locations = ngx_palloc(cf->temp_pool,
                                sizeof(ngx_http_location_queue_t));
        ngx_queue_init(*locations);
    }

    lq = ngx_palloc(cf->temp_pool, sizeof(ngx_http_location_queue_t));

    if (clcf->exact_match
#if (NGX_PCRE)
        || clcf->regex
#endif
        || clcf->named || clcf->noname)
    {
        lq->exact = clcf;
        lq->inclusive = NULL;

    } else {
        lq->exact = NULL;
        lq->inclusive = clcf;
    }

    lq->name = &clcf->name;
    lq->file_name = cf->conf_file->file.name.data;
    lq->line = cf->conf_file->line;

    ngx_queue_init(&lq->list);

    ngx_queue_insert_tail(*locations, &lq->queue);

    return NGX_OK;
}
```

比较简单，没啥好说的。一个问题就是为什么检查了`clcf->exact_match`之后还检查`clcf->regex`,
这俩不应该是互斥的么？（TODO）


## 总结

* `ngx_cycle_t::conf_ctx`存储的是所有核心模块的配置项，其他类型的模块由对应的核心模块自己
管理。
* HTTP 配置项有**可出现区域**和**最小生效粒度**这两个概念。
* 如果一个 HTTP 配置项最小生效粒度是`location{...}`，那么这个配置项应该放在本模块的
`ngx_http_xxx_loc_conf_t`结构体中，至于它的可出现区域，则需要根据其意义来确定。
