# content handler

nginx 将 HTTP 请求的流程分为 11 个 phase，其中每个 phase 都是一个 handler 的数组，其中`NGX_HTTP_CONTENT_PHASE`是我们日常开发 HTTP 模块时最常接触的阶段，这个阶段比较特殊。每个`ngx_http_core_loc_conf_t`中都有一个`handler`，被称为 content handler，此外还有一个 handler 的数组，这俩是互斥的，当`clcf->handler`有值时，所有处于 content phase 的请求都会路由到`clcf->handler`来处理，否则的话，请求由`NGX_HTTP_CONTENT_PHASE`中的 handler 数组来处理。

## 如何设置 content handler

一般在模块的核心命令的 handler 来处理，

```c
static ngx_http_command_t  ngx_http_flv_commands[] = {

    { ngx_string("flv"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_CONF_NOARGS,
      0,
      ngx_http_flv,
      NGX_HTTP_LOC_CONF_OFFSET,
      NULL }
}


static char *
ngx_http_flv(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_flv_handler;

    return NGX_CONF_OK;
}
```

* `NGX_OK` — the request has been successfully processed, request must be routed to the next phase;
* `NGX_DECLINED` — request must be routed to the next handler;
* `NGX_AGAIN`, NGX_DONE — the request has been successfully processed, the request must be suspended until some event (e.g., subrequest finishes, socket becomes writeable or timeout occurs) and handler must be called again;
* `NGX_ERROR`, NGX_HTTP_… — an error has occurred while processing the request.

## 参考

[nginx phases]_(http://www.nginxguts.com/phases/)
