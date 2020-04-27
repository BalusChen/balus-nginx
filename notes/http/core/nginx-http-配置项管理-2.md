# nginx HTTP 配置项管理（二）

前面讲了 HTTP 模块配置项是如何进行管理的，这是主要内容。这里就来总结一下其他几个也比较重要
的点。

## 配置项的合并

前面提到了，变量有最小生效粒度和可出现区域这两个概念，这俩概念并不是一一对应的，比如最小生效
粒度为`location`的指令可能同时出现在`http/server/location`块下，所以等一层一层将其合并到
其最小生效粒度（这里指的就是`location`）上去。

### srv conf 的合并

srv conf 可能同时在`http{...}`和`server{...}`中出现，但是其最小生效粒度为`server{...}`，
所以最终是要把它给 merge 到`server{...}`层面。

```c
static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    /* create main/srv/loc conf */
    ...

    /* pre configuration */
    ...

    /* parse inside the http{...} block */

    cf->module_type = NGX_HTTP_MODULE;
    cf->cmd_type = NGX_HTTP_MAIN_CONF;
    rv = ngx_conf_parse(cf, NULL);

    /* merge configuration */

    cmcf = ctx->main_conf[ngx_http_core_module.ctx_index];
    cscfp = cmcf->servers.elts;

    for (m = 0; cf->cycle->modules[m]; m++) {
        if (cf->cycle->modules[m]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = cf->cycle->modules[m]->ctx;
        mi = cf->cycle->modules[m]->ctx_index;
        if (module->init_main_conf) {
            rv = module->init_main_conf(cf, ctx->main_conf[mi]);
        }
        rv = ngx_http_merge_servers(cf, cmcf, module, mi);
    }

    /* create location trees */
    ...

    /* */
    ...
}
```

上面把解析`http`指令的代码中我们目前关心的部分给列出来了（删除了错误处理的部分）。可以看到
在解析完了`http{...}`块内部的的所有指令之后，初始化完 main conf 之后马上开始合并配置项。
这个是在`ngx_http_merge_servers`函数中做的。

```c
static char *
ngx_http_merge_servers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf,
    ngx_http_module_t *module, ngx_uint_t ctx_index)
{
    char                        *rv;
    ngx_uint_t                   s;
    ngx_http_conf_ctx_t         *ctx, saved;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_http_core_srv_conf_t   **cscfp;

    cscfp = cmcf->servers.elts;
    ctx = (ngx_http_conf_ctx_t *) cf->ctx;
    saved = *ctx;
    rv = NGX_CONF_OK;

    for (s = 0; s < cmcf->servers.nelts; s++) {

        /* merge the server{}s' srv_conf's */

        ctx->srv_conf = cscfp[s]->ctx->srv_conf;

        if (module->merge_srv_conf) {
            rv = module->merge_srv_conf(cf, saved.srv_conf[ctx_index],
                                        cscfp[s]->ctx->srv_conf[ctx_index]);
        }

        if (module->merge_loc_conf) {

            /* merge the server{}'s loc_conf */

            ctx->loc_conf = cscfp[s]->ctx->loc_conf;

            rv = module->merge_loc_conf(cf, saved.loc_conf[ctx_index],
                                        cscfp[s]->ctx->loc_conf[ctx_index]);

            /* merge the locations{}' loc_conf's */

            clcf = cscfp[s]->ctx->loc_conf[ngx_http_core_module.ctx_index];

            rv = ngx_http_merge_locations(cf, clcf->locations,
                                          cscfp[s]->ctx->loc_conf,
                                          module, ctx_index);
        }
    }
}
```

对每个 HTTP 模块，都调用`ngx_htt_merge_servers`函数。这个函数检查所有的`server{...}`块，
对每个`server{...}`块都调用该模块的`merge_srv_conf`和`merge_loc_conf`回调。然后针对
这个模块和这个`server{...}`块，调用`ngx_http_merge_locations`函数。

这里有一个地方需要注意，在调用`merge_srv_conf`之前有一句`ctx->srv_conf = cscfp[s]->ctx->srv_conf`，
传给`ngx_http_merge_servers`函数的`cf->ctx`是在解析`http`指令时创建的`ngx_http_conf_ctx_t`。
所以其中的`main_conf/srv_conf/loc_conf`三个数组存储的分别是`main conf/srv conf/loc conf`
出现在`http{...}`块下的值（这里说的 loc conf 指的是最小生效粒度为`location{...}`的配置项）。
但是用户在写自己的`merge_srv_conf`函数时，按照常理，从`cf->ctx->srv_conf`中拿到的应该是
`srv conf`在`server{...}`块中设置的值，所以这里就做这个工作。

后面在调用`merge_loc_conf`之前设置`ctx->loc_conf = cscfp[s]->ctx->loc_conf;`也是出于
类似的考虑。

TODO: 上面这部分我不是特别确定，而且看了一些 HTTP 模块的`merge_srv_conf`，都没有用到`cf`
这个参数。

需要注意的是`merge_loc_conf`这个回调，在`ngx_http_merge_servers`和`ngx_http_merge_locations`
中各调用了一次。在`merge_servers`调用的那次是把在`http{...}`中出现的 loc conf 给 merge
到`server{...}`层面，后面在`merge_locations`中在 merge 到`locations{...}`层面。

### loc conf 的合并

在 srv conf 的合并中，也把出现在`http{...}`中的 loc conf 给 merge 到了`server{...}`，
所以 loc conf 的合并只需要和上一级 merge 就可以了。

```c
static char *
ngx_http_merge_locations(ngx_conf_t *cf, ngx_queue_t *locations,
    void **loc_conf, ngx_http_module_t *module, ngx_uint_t ctx_index)
{
    char                       *rv;
    ngx_queue_t                *q;
    ngx_http_conf_ctx_t        *ctx, saved;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_location_queue_t  *lq;

    if (locations == NULL) {
        return NGX_CONF_OK;
    }

    ctx = (ngx_http_conf_ctx_t *) cf->ctx;
    saved = *ctx;

    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        clcf = lq->exact ? lq->exact : lq->inclusive;
        ctx->loc_conf = clcf->loc_conf;

        rv = module->merge_loc_conf(cf, loc_conf[ctx_index],
                                    clcf->loc_conf[ctx_index]);

        rv = ngx_http_merge_locations(cf, clcf->locations, clcf->loc_conf,
                                      module, ctx_index);
    }

    *ctx = saved;
    return NGX_CONF_OK;
}
```

我们已经知道`server{...}`块下的所有`location{...}`是通过`ngx_http_core_loc_conf_t::locations`
串联起来的，所以顺着这个队列一个一个`location{...}`地去调用这个模块的`merge_loc_conf`就
可以了；而`location{...}`允许嵌套的，所以其中还需要递归调用`ngx_http_merge_locations`。

而在调用`merge_loc_conf`之前的`ctx->loc_conf = clcf->loc_conf;`赋值语句作用和前面说过
的是类似的，目的就是为了让用户在实现自己的`merge_loc_conf`回调时从`cf->ctx->loc_conf`参
数中看到的 loc conf 在本`location{...}`的 loc conf；而不是在`server{...}`中的 loc conf。

## 加速`server`和`location`的检索

`http{...}`下`server{...}`可以出现多个，要是根据`server_name`一个一个对比来确定该使用
哪个，就太花时间了；而`location{...}`不仅可以在`server{...}`出现多个，还允许嵌套，要是
顺着`ngx_queue_t`队列一个一个找下去也非常花时间，所以需要对这两个的检索进行加速。

这两者的加速/优化措施都是在`ngx_http_block`中做的。

```c
static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ...
    /* merge servers */
    ...

    /* create location trees */

    for (s = 0; s < cmcf->servers.nelts; s++) {

        clcf = cscfp[s]->ctx->loc_conf[ngx_http_core_module.ctx_index];

        if (ngx_http_init_locations(cf, cscfp[s], clcf) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        if (ngx_http_init_static_location_trees(cf, clcf) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    /* init phases */
    ...
    /* init headers in hash */
    ...
    /* post configuration */
    ...
    /* init http variables */
    ...
    /* init phase handlers */
    ...

    /* optimize servers */

    if (ngx_http_optimize_servers(cf, cmcf, cmcf->ports) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;

failed:

    *cf = pcf;
    return rv;
}
```
