# upstream 向下游发送响应

upstream 机制的使用受限是创建请求(使用`u->create_request`)，然后连接上游服务器，
发送客户请求，上游服务器接收到 nginx 的请求之后进行恢复，nginx 接收该回复，并
进行处理，最后按照需求将其转发到下游。

由于应用层协议的二段式设计(包头+包体)，首先 nginx 需要接收响应头，并进行处理(
比如有的 header 需要特殊处理，有的 header 需要转发给服务器所以设置到`ngx_http_request_t`
结构体中的`headers_out`链表中去)。

在接收并处理完上游发来的所有响应包头之后，开始处理上游发来的响应包体。对包体的处
理方式有 3 种：

* 不转发到下游
* 使用固定缓冲区转发数据到下游
* 使用大量缓冲区和临时文件的方式转发数据到下游

## 源码剖析

这些都是在`ngx_http_upstream_send_response`方法中完成的。

```c
static void
ngx_http_upstream_send_response(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ssize_t                    n;
    ngx_int_t                  rc;
    ngx_event_pipe_t          *p;
    ngx_connection_t          *c;
    ngx_http_core_loc_conf_t  *clcf;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->post_action) {
        ngx_http_upstream_finalize_request(r, u, rc);
        return;
    }

    u->header_sent = 1;

    if (u->upgrade) {
        ...
        return;
    }

    c = r->connection;

    if (r->header_only) {

        if (!u->buffering) {
            ngx_http_upstream_finalize_request(r, u, rc);
            return;
        }

        if (!u->cacheable && !u->store) {
            ngx_http_upstream_finalize_request(r, u, rc);
            return;
        }

        u->pipe->downstream_error = 1;
    }

    if (r->request_body && r->request_body->temp_file
        && r == r->main && !r->preserve_body
        && !u->conf->preserve_output)
    {
        ngx_pool_run_cleanup_file(r->pool, r->request_body->temp_file->file.fd);
        r->request_body->temp_file->file.fd = NGX_INVALID_FILE;
    }
```

### 向下游发送响应头

在`ngx_http_upstream_send_response`函数中可以知道响应头的发送是由`ngx_http_send_header`
函数完成的：

```c
ngx_int_t
ngx_http_send_header(ngx_http_request_t *r)
{

}
```

## 总结
