
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) Nginx, Inc.
 */

/*
 * NOTE: 理解 slice 的应用场景，nginx 此时作为 cache 存在，slice 在这里是把一个请求给
 *       分割为多个子请求，
 *
 *      server {
 *          listen 8000;
 *
 *          location /media {
 *              slice 1m;
 *
 *              proxy_cache cache;
 *              proxy_cache_key $uri$is_args$args$slice_range;
 *              proxy_set_header Range $slice_range;
 *              proxy_cache_valid 200 206 1h;
 *              proxy_pass http://127.0.0.1:9000;
 *          }
 *      }
 *
 *
 *      1. client 以 /media/hello.flv 首次请求 nginx，此时 nginx 缓存中还没有该文件，
 *         所以需要从 upstream 拉取，而根据 nginx.conf 中的配置 nginx 会在对 upstream
 *         的请求中加上 Range: $slice_range 头；获取 $slice_range 的变量的值时，会调
 *         用 ngx_http_slice_range_variable 函数， 它根据 client 请求中的 Range 头
 *         来以 slice 为边界来向 upstream 获取数据
 *      2. nginx 收到了 upstream 发送的响应，开始向 client 发送 header ，此时要经过
 *         ngx_http_slice_header_filter 函数， 这个 filter
 *      3. nginx 向 client 发送 body 时，需要经过 ngx_http_slice_body_filter 函数，
 *         这个 filter 主要做的是
 *
 * NOTE: 1. 首先通过 $slice_range 指定需要向 upstream 请求的数据范围（第一次时通过
 *          client request 计算出来，后面直接从 slice-ctx 中拿）
 *       2. 从 upstream 获取到数据之后，将该数据首先用 header filter 过滤一遍，此时
 *          slice header filter 会依据 upstream 返回的 Content-Range 来更新 slice
 *          ctx，为下一次向 upstream 发起 Range 请求做准备
 *       3. 然后将该数据用 body filter 过滤，在调用
 *          有可能指定的数据范围太大，或者是 upstream 不
 *          支持 Range，导致 upstream 直接返回了整个文件，也就是说状态码不是 PARTIAL，
 *          那么直接从
 *
 *
 * NOTE: 理解几个 HTTP HEADER:
 *       1. Range：客户端发送给服务器，指定需要的数据的范围，在这里可以是 client 发送给
 *                 nginx，也可以是 nginx 发送给 upstream
 *                * 格式: Range: bytes=START-END
 *                * 注意：闭区间；只要 client 指定了 Range HEADER，那么不论是 range
 *                       超出了文件的大小还是其他，server 都该返回 Content-Range
 *                       比如文件大小为 512B，指定的 Range 为 128-256, 0-513, 128-1024
 *                       或者是 1024-2048，server 都会返回 Content-Range。
 *       2. Content-Range：服务器对客户端 range 请求的响应，
 *                * 格式：Content-Range: bytes START-END/SIZE
 *                * 注意：闭区间，size表示的是 整个文件的大小
 *       3. Content-Offset
 *       4. Content-Length
 *
 * QUESTION: subrequest 向 upstream 请求来了数据之后，怎么回到 sub request 里面呢？
 *           subrequest 怎么回到 main request 呢？
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    size_t               size;
} ngx_http_slice_loc_conf_t;


typedef struct {
    off_t                start;
    off_t                end;
    ngx_str_t            range;
    ngx_str_t            etag;
    unsigned             last:1;
    unsigned             active:1;
    ngx_http_request_t  *sr;
} ngx_http_slice_ctx_t;


typedef struct {
    off_t                start;
    off_t                end;
    off_t                complete_length;
} ngx_http_slice_content_range_t;


static ngx_int_t ngx_http_slice_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_slice_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_slice_parse_content_range(ngx_http_request_t *r,
    ngx_http_slice_content_range_t *cr);
static ngx_int_t ngx_http_slice_range_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static off_t ngx_http_slice_get_start(ngx_http_request_t *r);
static void *ngx_http_slice_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_slice_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_slice_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_slice_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_slice_filter_commands[] = {

    { ngx_string("slice"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_slice_loc_conf_t, size),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_slice_filter_module_ctx = {
    ngx_http_slice_add_variables,          /* preconfiguration */
    ngx_http_slice_init,                   /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_slice_create_loc_conf,        /* create location configuration */
    ngx_http_slice_merge_loc_conf          /* merge location configuration */
};


ngx_module_t  ngx_http_slice_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_slice_filter_module_ctx,     /* module context */
    ngx_http_slice_filter_commands,        /* module directives */
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


static ngx_str_t  ngx_http_slice_range_name = ngx_string("slice_range");

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_int_t
ngx_http_slice_header_filter(ngx_http_request_t *r)
{
    off_t                            end;
    ngx_int_t                        rc;
    ngx_table_elt_t                 *h;
    ngx_http_slice_ctx_t            *ctx;
    ngx_http_slice_loc_conf_t       *slcf;
    ngx_http_slice_content_range_t   cr;

    /*
     * NOTE: ctx 是在解析 $slice_range 变量的时候设置进 ngx_http_request_t 中的，
     *       解析 $slice_range 会根据 client 发来的 Range header 设置 ctx->start，
     *       TODO:
     *
     * NOTE: 如果 request 中没有 ngx_http_slice_ctx_t，那么说明：
     *       1. nginx.conf 中没有使用到 $slice_range
     *       2. 或者，此请求为 subrequest
     */
    ctx = ngx_http_get_module_ctx(r, ngx_http_slice_filter_module);
    if (ctx == NULL) {
        return ngx_http_next_header_filter(r);
    }

    if (r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT) {

        /*
         * NOTE: upsteram 返回的 status != PARTIAL_CONTENT，如果这是 main request，
         *       而 upstream 本身就不支持 Range 从而返回了整个文件，我们把 main request
         *       中的 slice ctx 置为 NULL，这样后续继续调用本 filter 时，就可以及早退
         *       出了
         */
        if (r == r->main) {
            ngx_http_set_ctx(r, NULL, ngx_http_slice_filter_module);
            return ngx_http_next_header_filter(r);
        }

        /*
         * NOTE: 而对于 subrequest，由于只有在 main request 收到了上游返回的 PARTIAL
         *       （即明确了它支持 Range 请求）之后才有可能会创建 subrequest，所以此时
         *       upstream 理应返回 PARTIAL_CONTENT，而这里没有返回，那么肯定是有问题。
         */
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "unexpected status code %ui in slice response",
                      r->headers_out.status);
        return NGX_ERROR;
    }

    /* ATTENTION: 走到这里说明 status == PARTIAL_CONTENT */

    h = r->headers_out.etag;

    /*
     * NOTE: 在第一次收到 upstream 的响应时，如果 upstream 返回了 etag，那么就把它放到
     *       ctx 中（也就是下面的 if (h) { ctx->tag = h->value; }）这句。后面继续收到
     *       upstream 的响应时，就可以通过匹配这两个 etag 来检查 upstream 多次发送的是
     *       不是同一个文件（或者说 entity）
     */
    if (ctx->etag.len) {
        if (h == NULL
            || h->value.len != ctx->etag.len
            || ngx_strncmp(h->value.data, ctx->etag.data, ctx->etag.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "etag mismatch in slice response");
            return NGX_ERROR;
        }
    }

    // balus: 是否可以改为 if (h && !ctx->etag.len) { ctx->etag = h->value; }
    if (h) {
        ctx->etag = h->value;
    }

    /*
     * NOTE: 解析 upstream 返回的 Content-Range 头部，这个响应头部反应的是这个数据片段
     *       在整个文件中的位置，其语法如下：
     *       Content-Range: bytes start-end/size
     *       如果 size 未知的话，则为 *，注意 size 表示的是文件的总大小，而不是本次传输
     *       的数据大小，所以通过这个 size 字段，我们可以知道是否还需要发送 subrequest
     *       可以参考：https://developer.mozilla.org/zh-CN/docs/Web/HTTP/Headers/Content-Range
     *
     * ATTENTION: upstream 返回的是 [start, end]，但是 nginx 为了方便计算，而将这个
     *            闭区间解析为了 [start, end + 1) 左闭右开区间
     */
    if (ngx_http_slice_parse_content_range(r, &cr) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "invalid range in slice response");
        return NGX_ERROR;
    }

    /*
     * NOTE：由于不知道 complete_length 的值，我们无法确定是否还需要继续发送 subrequest，
     *       所以最好就是返回出错。
     */
    if (cr.complete_length == -1) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "no complete length in slice response");
        return NGX_ERROR;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http slice response range: %O-%O/%O",
                   cr.start, cr.end, cr.complete_length);

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_slice_filter_module);

    end = ngx_min(cr.start + (off_t) slcf->size, cr.complete_length);

    if (cr.start != ctx->start || cr.end != end) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "unexpected range in slice response: %O-%O",
                      cr.start, cr.end);
        return NGX_ERROR;
    }

    /*
     * NOTE: 更新 slice ctx，为下一次请求 upstream 做准备（即获取 $slice_range 变量
     *       的值）这里只更新了 ctx->start，其实只需要这个就可以了，毕竟我们每次都是以
     *       slice 为单位请求 upstream，那么 ctx->end 的作用是什么呢？
     *
     * ATTENTION: 由于 nginx 将 upstream 返回的 [start, end] 解析为了 [start, end+1)
     *            即 cr.end = upstream.content_range.end + 1，所以这里不用再+1了
     *
     * NOTE: active 标志位是给 slice subrequest 重定向至新的 location 的情况使用的。
     *       subrequest 重定向至新的 location 时，其 context 就丢失了。
     */
    ctx->start = end;
    ctx->active = 1;

    /*
     * QUESTION: 为什么设置为 NGX_HTTP_OK，本应该是 NGX_HTTP_PARTIAL_CONTENT 啊？
     *
     * NOTE: 在后面执行的 range header filter 只会处理 status == HTTP_OK 的响应，
     *       而 slice 模块需要 range 模块的支持，所以需要在这里首先将 status 设置为 OK，
     *       下面执行 ngx_http_next_header_filter(r) 才可以被 range header filter
     *       处理，而 range header filter 会根据(TD)
     */
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.status_line.len = 0;
    // QUESTION: content_length 和 content_length_n 的区别？
    r->headers_out.content_length_n = cr.complete_length;
    r->headers_out.content_offset = cr.start;
    // NOTE: hash 设置为 0 表示这个 header 无效
    r->headers_out.content_range->hash = 0;
    r->headers_out.content_range = NULL;

    /*
     * QUESTION: 下面三个字段的含义？
     *
     * NOTE:
     *      1. allow_ranges
     *      2. subrequest_ranges
     *      3. single_range
     */
    r->allow_ranges = 1;
    r->subrequest_ranges = 1;
    r->single_range = 1;

    // NOTE: 执行完其余的 header filter
    rc = ngx_http_next_header_filter(r);

    if (r != r->main) {
        return rc;
    }

    /* ATTENTION: 下面的操作都针对 main request 进行 */

    r->preserve_body = 1;

    /*
     * QUESTION: 前面不是已经把 != PARTIAL_CONTENT 的情况排除了么？难道其他 filter 会
     *           修改 status？
     */
    if (r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT) {

        /*
         * NOTE: 如果经历了其他 header filter 的处理之后 status 回到了 PARTIAL_CONTENT，
         *       那么说明
         *
         * QUESTION: nginx 不是以 [ctx->start, ctx->start + slice] 为 Range 请求
         *           upstream 么？前面 content_offset 设置的也是 cr.start 啊，我理解
         *           ctx->start 和 cr.start，也就是 content_offset 应该是相等的啊，
         *           为什么会需要用 + slcf->size 来进行比较呢？
         *
         * TODO: 在 range module 的 ngx_http_range_singlepart_header 函数中有可能
         *       对这个字段做出修改，后面看了 range module 之后回过来理解这里
         */
        if (ctx->start + (off_t) slcf->size <= r->headers_out.content_offset) {
            ctx->start = slcf->size
                         * (r->headers_out.content_offset / slcf->size);
        }

        ctx->end = r->headers_out.content_offset
                   + r->headers_out.content_length_n;

    } else {
        ctx->end = cr.complete_length;
    }

    return rc;
}


/*
 * NOTE: slice body filter 的作用
 */

static ngx_int_t
ngx_http_slice_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_int_t                   rc;
    ngx_chain_t                *cl;
    ngx_http_slice_ctx_t       *ctx;
    ngx_http_slice_loc_conf_t  *slcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_slice_filter_module);

    /*
     * NOTE: 对于 subrequest，他们只需要将其从 upstream 接收到的内容发送出去就可以，
     *       所以直接调用下一个 body filter。而对于 main request，它需要按顺序派生
     *       subrequest 请求 upstream
     */
    if (ctx == NULL || r != r->main) {
        return ngx_http_next_body_filter(r, in);
    }

    /* ATTENTION: 以下只有 main request 执行 */

    /*
     * QUESTION: 这个 for 循环是在干什么？
     *
     * NOTE:
     */
    for (cl = in; cl; cl = cl->next) {
        if (cl->buf->last_buf) {
            cl->buf->last_buf = 0;
            cl->buf->last_in_chain = 1;
            cl->buf->sync = 1;
            ctx->last = 1;
            /*
             * balus: 这里可以加一个 break 么？一个 chain 里面会有多个 last_buf 置位
             *        的 buf 么？
             */
        }
    }

    rc = ngx_http_next_body_filter(r, in);

    /*
     * QUESTION: ctx->last 表示什么？
     */
    if (rc == NGX_ERROR || !ctx->last) {
        return rc;
    }

    /*
     * NOTE: sr->done == 1 表示子请求接收完了全部的响应
     */
    if (ctx->sr && !ctx->sr->done) {
        return rc;
    }

    if (!ctx->active) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "missing slice response");
        return NGX_ERROR;
    }

    /*
     * QUESTION: 这是什么情况？
     *
     * NOTE: 表示完整文件已经发送完成
     */
    if (ctx->start >= ctx->end) {
        ngx_http_set_ctx(r, NULL, ngx_http_slice_filter_module);
        ngx_http_send_special(r, NGX_HTTP_LAST);
        return rc;
    }

    if (r->buffered) {
        return rc;
    }

    /*
     * NOTE: 这里创建 subrequest，在 main request 的数据全部发送给 client 之后执行
     *       默认生成子请求从 server_rewrite 阶段执行并跳过 access 阶段，这里
     *       NGX_HTTP_SUBREQUEST_CLONE 使生成的子请求从主请求的当前阶段（即content阶段）
     *       开始执行
     *
     * QUESTION: 什么时候会调度 subrequest 呢？
     */
    if (ngx_http_subrequest(r, &r->uri, &r->args, &ctx->sr, NULL,
                            NGX_HTTP_SUBREQUEST_CLONE)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(ctx->sr, ctx, ngx_http_slice_filter_module);

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_slice_filter_module);

    ctx->range.len = ngx_sprintf(ctx->range.data, "bytes=%O-%O", ctx->start,
                                 ctx->start + (off_t) slcf->size - 1)
                     - ctx->range.data;

    ctx->active = 0;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http slice subrequest: \"%V\"", &ctx->range);

    return rc;
}


/*
 * NOTE: 解析 upstream 返回的 Content-Range 头，其语法如下：
 *       Content-Range: bytes start-end/size
 *       其中 size 表示整个文件的大小
 */
static ngx_int_t
ngx_http_slice_parse_content_range(ngx_http_request_t *r,
    ngx_http_slice_content_range_t *cr)
{
    off_t             start, end, complete_length, cutoff, cutlim;
    u_char           *p;
    ngx_table_elt_t  *h;

    h = r->headers_out.content_range;

    if (h == NULL
        || h->value.len < 7
        || ngx_strncmp(h->value.data, "bytes ", 6) != 0)
    {
        return NGX_ERROR;
    }

    p = h->value.data + 6;

    cutoff = NGX_MAX_OFF_T_VALUE / 10;
    cutlim = NGX_MAX_OFF_T_VALUE % 10;

    start = 0;
    end = 0;
    complete_length = 0;

    while (*p == ' ') { p++; }

    if (*p < '0' || *p > '9') {
        return NGX_ERROR;
    }

    while (*p >= '0' && *p <= '9') {
        if (start >= cutoff && (start > cutoff || *p - '0' > cutlim)) {
            return NGX_ERROR;
        }

        start = start * 10 + (*p++ - '0');
    }

    while (*p == ' ') { p++; }

    if (*p++ != '-') {
        return NGX_ERROR;
    }

    while (*p == ' ') { p++; }

    if (*p < '0' || *p > '9') {
        return NGX_ERROR;
    }

    while (*p >= '0' && *p <= '9') {
        if (end >= cutoff && (end > cutoff || *p - '0' > cutlim)) {
            return NGX_ERROR;
        }

        end = end * 10 + (*p++ - '0');
    }

    /*
     * NOTE: nginx 的惯例，本来 upstream 返回的 Content-Range 是闭区间，但是 nginx
     *       将其转换为左闭右开区间，所以将 end 加 1，构成 [start, end+1)，这个在 range
     *       模块处理 client request 中的 Range HEADER 时也用到了。我感觉是便于计算，
     *       这样计算范围大小就不用 end - start + 1 频繁地 + 1 了
     */
    end++;

    while (*p == ' ') { p++; }

    if (*p++ != '/') {
        return NGX_ERROR;
    }

    while (*p == ' ') { p++; }

    if (*p != '*') {
        if (*p < '0' || *p > '9') {
            return NGX_ERROR;
        }

        while (*p >= '0' && *p <= '9') {
            if (complete_length >= cutoff
                && (complete_length > cutoff || *p - '0' > cutlim))
            {
                return NGX_ERROR;
            }

            complete_length = complete_length * 10 + (*p++ - '0');
        }

    } else {
        complete_length = -1;
        p++;
    }

    while (*p == ' ') { p++; }

    if (*p != '\0') {
        return NGX_ERROR;
    }

    cr->start = start;
    cr->end = end;
    cr->complete_length = complete_length;

    return NGX_OK;
}

/*
 * NOTE: 获取 $slice_range 的值，一般会用在 proxy_cache_key 和 proxy_set_header 中：
 *           proxy_cache_key $uri$is_args$args$slice_range;
 *           proxy_set_header Range $slice_range;
 *       第一次获取 $slice_range 的值时，此时 request 中还没有 slice_ctx，需要从 client
 *       request 中中获取变量的值，具体操作流程就是检查 client request 中是否有 Range
 *       HEADER，如果有的话，则根据 Range HEADER 以及 slice 指令的值来获取 $slice_range
 *       的值；没有的话，$slice_range 就是 "bytes 0-slice-1；这里需要注意，$slice_range
 *       都是以 slice 指令的值为单位。
 *       不是第一次获取 $slice_range 的话，request 中就有了 slice_ctx 了，直接从里面拿
 *       就可以了。
 */
static ngx_int_t
ngx_http_slice_range_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                     *p;
    ngx_http_slice_ctx_t       *ctx;
    ngx_http_slice_loc_conf_t  *slcf;

    ctx = ngx_http_get_module_ctx(r, ngx_http_slice_filter_module);

    if (ctx == NULL) {
        if (r != r->main || r->headers_out.status) {
            v->not_found = 1;
            return NGX_OK;
        }

        slcf = ngx_http_get_module_loc_conf(r, ngx_http_slice_filter_module);

        /*
         * NOTE: 说明没有在 nginx.conf 使用 `slice;` 指令
         */
        if (slcf->size == 0) {
            v->not_found = 1;
            return NGX_OK;
        }

        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_slice_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_slice_filter_module);

        p = ngx_pnalloc(r->pool, sizeof("bytes=-") - 1 + 2 * NGX_OFF_T_LEN);
        if (p == NULL) {
            return NGX_ERROR;
        }

        /*
         * NOTE: 必须以 slice 的大小为边界
         *       而且需要注意，这里并没有设置 ctx->end
         */
        ctx->start = slcf->size * (ngx_http_slice_get_start(r) / slcf->size);

        ctx->range.data = p;
        ctx->range.len = ngx_sprintf(p, "bytes=%O-%O", ctx->start,
                                     ctx->start + (off_t) slcf->size - 1)
                         - p;
    }

    v->data = ctx->range.data;
    v->valid = 1;
    v->not_found = 0;
    v->no_cacheable = 1;
    v->len = ctx->range.len;

    return NGX_OK;
}


static off_t
ngx_http_slice_get_start(ngx_http_request_t *r)
{
    off_t             start, cutoff, cutlim;
    u_char           *p;
    ngx_table_elt_t  *h;

    /*
     * NOTE: If-Range 头必须和 Range 头一起使用，表示 "如果 entity 没有变化的话，把我
     *       缺失的部分传给我，否则把整个传给我"，其语法如下：
     *       If-Range : (etag | http-date)
     *       如果 client 对于该 entity 没有 etag，而只有一个 modified time，那么可以用该
     *       modified time，server 可以很容易地区分 etag 和 http-date
     *       REF: https://www.w3.org/Protocols/rfc2616/rfc2616-sec14.html#sec14.27
     *
     * QUESTION: 那么这里如果设置和 If-Range 头，则直接返回 0，这是为什么？
     */
    if (r->headers_in.if_range) {
        return 0;
    }

    h = r->headers_in.range;

    if (h == NULL
        || h->value.len < 7
        || ngx_strncasecmp(h->value.data, (u_char *) "bytes=", 6) != 0)
    {
        return 0;
    }

    p = h->value.data + 6;

    /*
     * NOTE: bytes=500-600,701-800 这种带多个区间的，Range 头部也是支持的，
     *
     * QUESTION: nginx 不支持多个区间的 Range 头部，为什么？
     */
    if (ngx_strchr(p, ',')) {
        return 0;
    }

    while (*p == ' ') { p++; }

    // NOTE: 'Range: bytes=-500' 这种格式
    if (*p == '-') {
        return 0;
    }

    cutoff = NGX_MAX_OFF_T_VALUE / 10;
    cutlim = NGX_MAX_OFF_T_VALUE % 10;

    start = 0;

    while (*p >= '0' && *p <= '9') {
        if (start >= cutoff && (start > cutoff || *p - '0' > cutlim)) {
            return 0;
        }

        start = start * 10 + (*p++ - '0');
    }

    return start;
}


static void *
ngx_http_slice_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_slice_loc_conf_t  *slcf;

    slcf = ngx_palloc(cf->pool, sizeof(ngx_http_slice_loc_conf_t));
    if (slcf == NULL) {
        return NULL;
    }

    slcf->size = NGX_CONF_UNSET_SIZE;

    return slcf;
}


static char *
ngx_http_slice_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_slice_loc_conf_t *prev = parent;
    ngx_http_slice_loc_conf_t *conf = child;

    ngx_conf_merge_size_value(conf->size, prev->size, 0);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_slice_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_slice_range_name, 0);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_slice_range_variable;

    return NGX_OK;
}


static ngx_int_t
ngx_http_slice_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_slice_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_slice_body_filter;

    return NGX_OK;
}
