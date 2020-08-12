
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static ngx_int_t ngx_http_postpone_filter_add(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_postpone_filter_in_memory(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_postpone_filter_init(ngx_conf_t *cf);


static ngx_http_module_t  ngx_http_postpone_filter_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_postpone_filter_init,         /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_postpone_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_postpone_filter_module_ctx,  /* module context */
    NULL,                                  /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


/*
 * ATTENTION: 注意区分 postponed 和 posted_request 这两个链表
 */
static ngx_int_t
ngx_http_postpone_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_connection_t              *c;
    ngx_http_postponed_request_t  *pr;

    c = r->connection;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http postpone filter \"%V?%V\" %p", &r->uri, &r->args, in);

    if (r->subrequest_in_memory) {
        return ngx_http_postpone_filter_in_memory(r, in);
    }

    /*
     * NOTE: 如果本 request 不是 Current-Active-Request 的话，说明它不能发送数据，
     *       所以如果 in 链表（当前产生的数据，可能为 NULL）不为空，我们需要临时保存它，
     *       延迟发送。
     *       这里所说的临时保存，就是将 in 链表作为一个数据节点，保存在本 request 的
     *       postponed 链表上。
     */
    if (r != c->data) {

        if (in) {
            if (ngx_http_postpone_filter_add(r, in) != NGX_OK) {
                return NGX_ERROR;
            }

            return NGX_OK;
        }

#if 0
        /* TODO: SSI may pass NULL */
        ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                      "http postpone filter NULL inactive request");
#endif

        return NGX_OK;
    }

    /*
     * NOTE: 如果这个是 CAR，并且其 postponed 链表为空，说明它可以发送数据了。而且正好本
     *       次待发送的数据（in 链表）或者之前没有发送完的数据（c->buffered）不为空，那
     *       么发送他们（即调用之后的 body filter）。
     */
    if (r->postponed == NULL) {

        if (in || c->buffered) {
            return ngx_http_next_body_filter(r->main, in);
        }

        return NGX_OK;
    }

    /*
     * NOTE: 如果这个请求是 CAR，并且 postponed 链表不为空，而且本次有需要发送的数据，
     *       那么由于它还存在 subrequest 或者尚未发送完的数据（即 postponed 链表节点），
     *       所以它的数据不能发送，而需要放在 postponed 链表，作为一个数据节点。
     */
    if (in) {
        if (ngx_http_postpone_filter_add(r, in) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    /*
     * NOTE: CAR && postponed != NULL && in == NULL（或者 in != NULL，但是在前面的
     *       步骤中已经把 in 中的数据作为一个数据节点存入 postponed 链表了）
     *       此时可以处理其 postponed 链表
     */
    do {
        pr = r->postponed;

        /*
         * NOTE: 如果是一个 request 节点，那么就将其加入到 main request 的
         *       posted_requests 链表尾部
         */
        if (pr->request) {

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter wake \"%V?%V\"",
                           &pr->request->uri, &pr->request->args);

            r->postponed = pr->next;

            /*
             * NOTE:
             */
            c->data = pr->request;

            return ngx_http_post_request(pr->request, NULL);
        }

        /*
         * NOTE：如果是一个数据节点，那么发送它（也就是调用后面的 body filter）
         */

        if (pr->out == NULL) {
            ngx_log_error(NGX_LOG_ALERT, c->log, 0,
                          "http postpone filter NULL output");

        } else {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http postpone filter output \"%V?%V\"",
                           &r->uri, &r->args);

            /*
             * QUESTION: 为什么第一个参数要设置为 r->main
             */
            if (ngx_http_next_body_filter(r->main, pr->out) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }

        r->postponed = pr->next;

    } while (r->postponed);

    return NGX_OK;
}


/*
 * NOTE: 往 postponed 链表中添加数据
 */
static ngx_int_t
ngx_http_postpone_filter_add(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_postponed_request_t  *pr, **ppr;

    if (r->postponed) {
        for (pr = r->postponed; pr->next; pr = pr->next) { /* void */ }

        /*
         * NOTE: 如果最后一个节点是数据节点，那么就可以把新增的数据追加到这个节点上面，
         *       而不用新创建一个数据节点。
         */
        if (pr->request == NULL) {
            goto found;
        }

        ppr = &pr->next;

    } else {
        ppr = &r->postponed;
    }

    /*
     * NOTE: 从这个函数可以看出来，postponed 链表不仅仅可以存 subrequest，还可以存
     *       数据（数据节点）。
     */
    pr = ngx_palloc(r->pool, sizeof(ngx_http_postponed_request_t));
    if (pr == NULL) {
        return NGX_ERROR;
    }

    *ppr = pr;

    pr->request = NULL;
    pr->out = NULL;
    pr->next = NULL;

found:

    // NOTE: 把 in 中的 buf 给拷贝到 pr->out 中，其实就是指针的拷贝
    if (ngx_chain_add_copy(r->pool, &pr->out, in) == NGX_OK) {
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_postpone_filter_in_memory(ngx_http_request_t *r, ngx_chain_t *in)
{
    size_t                     len;
    ngx_buf_t                 *b;
    ngx_connection_t          *c;
    ngx_http_core_loc_conf_t  *clcf;

    c = r->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http postpone filter in memory");

    if (r->out == NULL) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        if (r->headers_out.content_length_n != -1) {
            len = r->headers_out.content_length_n;

            if (len > clcf->subrequest_output_buffer_size) {
                ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "too big subrequest response: %uz", len);
                return NGX_ERROR;
            }

        } else {
            len = clcf->subrequest_output_buffer_size;
        }

        b = ngx_create_temp_buf(r->pool, len);
        if (b == NULL) {
            return NGX_ERROR;
        }

        b->last_buf = 1;

        r->out = ngx_alloc_chain_link(r->pool);
        if (r->out == NULL) {
            return NGX_ERROR;
        }

        r->out->buf = b;
        r->out->next = NULL;
    }

    b = r->out->buf;

    for ( /* void */ ; in; in = in->next) {

        if (ngx_buf_special(in->buf)) {
            continue;
        }

        len = in->buf->last - in->buf->pos;

        if (len > (size_t) (b->end - b->last)) {
            ngx_log_error(NGX_LOG_ERR, c->log, 0,
                          "too big subrequest response");
            return NGX_ERROR;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http postpone filter in memory %uz bytes", len);

        b->last = ngx_cpymem(b->last, in->buf->pos, len);
        in->buf->pos = in->buf->last;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_postpone_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_postpone_filter;

    return NGX_OK;
}
