
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_LIMIT_CONN_PASSED            1
#define NGX_HTTP_LIMIT_CONN_REJECTED          2
#define NGX_HTTP_LIMIT_CONN_REJECTED_DRY_RUN  3


typedef struct {
    u_char                        color;
    u_char                        len;
    u_short                       conn;
    u_char                        data[1];
} ngx_http_limit_conn_node_t;


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_rbtree_node_t            *node;
} ngx_http_limit_conn_cleanup_t;


/*
 * QUESTION: ngx_rbtree_t 里面不是已经有一个 sentinel 了么？
 *           为什么这里还要在同级加一个？
 */
typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
} ngx_http_limit_conn_shctx_t;


typedef struct {
    ngx_http_limit_conn_shctx_t  *sh;
    ngx_slab_pool_t              *shpool;
    ngx_http_complex_value_t      key;
} ngx_http_limit_conn_ctx_t;


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_uint_t                    conn;
} ngx_http_limit_conn_limit_t;


typedef struct {
    // NOTE: 每个元素都是 ngx_http_limit_conn_ctx_t
    ngx_array_t                   limits;
    ngx_uint_t                    log_level;
    ngx_uint_t                    status_code;
    ngx_flag_t                    dry_run;
} ngx_http_limit_conn_conf_t;


static ngx_rbtree_node_t *ngx_http_limit_conn_lookup(ngx_rbtree_t *rbtree,
    ngx_str_t *key, uint32_t hash);
static void ngx_http_limit_conn_cleanup(void *data);
static ngx_inline void ngx_http_limit_conn_cleanup_all(ngx_pool_t *pool);

static ngx_int_t ngx_http_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void *ngx_http_limit_conn_create_conf(ngx_conf_t *cf);
static char *ngx_http_limit_conn_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_limit_conn_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_limit_conn_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_limit_conn_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_limit_conn_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_limit_conn_commands[] = {

    { ngx_string("limit_conn_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_limit_conn_zone,
      0,
      0,
      NULL },

    { ngx_string("limit_conn"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_limit_conn,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("limit_conn_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_conf_t, log_level),
      &ngx_http_limit_conn_log_levels },

    { ngx_string("limit_conn_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_conf_t, status_code),
      &ngx_http_limit_conn_status_bounds },

    { ngx_string("limit_conn_dry_run"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_conn_conf_t, dry_run),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_limit_conn_module_ctx = {
    ngx_http_limit_conn_add_variables,     /* preconfiguration */
    ngx_http_limit_conn_init,              /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_limit_conn_create_conf,       /* create location configuration */
    ngx_http_limit_conn_merge_conf         /* merge location configuration */
};


ngx_module_t  ngx_http_limit_conn_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_conn_module_ctx,       /* module context */
    ngx_http_limit_conn_commands,          /* module directives */
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


static ngx_http_variable_t  ngx_http_limit_conn_vars[] = {

    { ngx_string("limit_conn_status"), NULL,
      ngx_http_limit_conn_status_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

      ngx_http_null_variable
};


static ngx_str_t  ngx_http_limit_conn_status[] = {
    ngx_string("PASSED"),
    ngx_string("REJECTED"),
    ngx_string("REJECTED_DRY_RUN")
};


static ngx_int_t
ngx_http_limit_conn_handler(ngx_http_request_t *r)
{
    size_t                          n;
    uint32_t                        hash;
    ngx_str_t                       key;
    ngx_uint_t                      i;
    ngx_rbtree_node_t              *node;
    ngx_pool_cleanup_t             *cln;
    ngx_http_limit_conn_ctx_t      *ctx;
    ngx_http_limit_conn_node_t     *lc;
    ngx_http_limit_conn_conf_t     *lccf;
    ngx_http_limit_conn_limit_t    *limits;
    ngx_http_limit_conn_cleanup_t  *lccln;

    if (r->main->limit_conn_status) {
        return NGX_DECLINED;
    }

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_limit_conn_module);
    limits = lccf->limits.elts;

    /*
     * NOTE: 一个 location 下可能有多个 limit_conn（但是不同的 key）
     *       所以这里选择
     *
     * QUESTION: 有一个疑惑，handler 是在 main/srv/loc 哪里发挥作用的？
     *           由谁来控制呢？
     */
    for (i = 0; i < lccf->limits.nelts; i++) {
        ctx = limits[i].shm_zone->data;

        /*
         * NOTE: 前面在解析 `limit_conn_zone key zone=name:size` 配置项时把
         *       key 给编译成了一个 script code
         *       现在遇到了一个请求，就把在该请求下的具体的 key 的值给解析出来
         */
        if (ngx_http_complex_value(r, &ctx->key, &key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (key.len == 0) {
            continue;
        }

        if (key.len > 255) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" key "
                          "is more than 255 bytes: \"%V\"",
                          &ctx->key.value, &key);
            continue;
        }

        r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_PASSED;

        /*
         * NOTE: 假如 limit_conn_zone 配置的 key 是 $binary_addr
         *       那么每次连接该变量对应的值都是不同的，所以可以根据这个
         *       变量来判断这个连接出现的频次
         *
         * QUESTION: 感觉这个限制的对象是和 key 相关的，加入我把 key 设置为 $uri
         *           那么这个限制的就是请求了，而不是连接。但是设置为 $binary_addr
         *           这个是连接维度上的，所以限制的就是连接了
         */
        hash = ngx_crc32_short(key.data, key.len);

        ngx_shmtx_lock(&ctx->shpool->mutex);

        /*
         * NOTE: 这里 lookup 操作是干嘛？
         *
         * QUESTION: 前面 ngx_crc32_short 可以保证不同输入会哈希成不同的输出么？
         *
         * NOTE: 不必保证，因为这里的 hash 只是便于比较，因为不同的 hash 值说明 key
         *       肯定是不同的，如果碰到了相同的 hash 值，那就再用 key 比较就可以了
         */
        node = ngx_http_limit_conn_lookup(&ctx->sh->rbtree, &key, hash);

        if (node == NULL) {

            /*
             * QUESTION: 下面这句是干啥？
             *
             * NOTE: 这句要和下面的 lc = (ngx_http_limit_conn_node_t *) &node->color;
             *       一起看 ngx_rbtree_note_t 的 color 成员此时放的是一个
             *       ngx_http_limit_conn_node_t 因为 ngx_http_limit_conn_node_t
             *       的第一个成员也是 u_char  color; 所以不用担心 color 没了
             *       然后 ngx_http_limit_conn_node_t 的 data 成员作为最后一个字段
             *       其实就是一个占位符，相当于柔性数组，在这里用来存储 key
             */
            n = offsetof(ngx_rbtree_node_t, color)
                + offsetof(ngx_http_limit_conn_node_t, data)
                + key.len;

            /*
             * QUESTION: alloc_locked 是什么意思？
             */
            node = ngx_slab_alloc_locked(ctx->shpool, n);

            if (node == NULL) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                ngx_http_limit_conn_cleanup_all(r->pool);

                /*
                 * QUESTION: dry_run 的含义是什么？
                 */
                if (lccf->dry_run) {
                    r->main->limit_conn_status =
                                          NGX_HTTP_LIMIT_CONN_REJECTED_DRY_RUN;
                    return NGX_DECLINED;
                }

                r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_REJECTED;

                return lccf->status_code;
            }

            lc = (ngx_http_limit_conn_node_t *) &node->color;

            node->key = hash;
            lc->len = (u_char) key.len;
            lc->conn = 1;
            ngx_memcpy(lc->data, key.data, key.len);

            ngx_rbtree_insert(&ctx->sh->rbtree, node);

        } else {

            lc = (ngx_http_limit_conn_node_t *) &node->color;

            /*
             * NOTE: 真正的连接限制操作在这里
             */
            if ((ngx_uint_t) lc->conn >= limits[i].conn) {

                ngx_shmtx_unlock(&ctx->shpool->mutex);

                ngx_log_error(lccf->log_level, r->connection->log, 0,
                              "limiting connections%s by zone \"%V\"",
                              lccf->dry_run ? ", dry run," : "",
                              &limits[i].shm_zone->shm.name);

                ngx_http_limit_conn_cleanup_all(r->pool);

                if (lccf->dry_run) {
                    r->main->limit_conn_status =
                                          NGX_HTTP_LIMIT_CONN_REJECTED_DRY_RUN;
                    return NGX_DECLINED;
                }

                r->main->limit_conn_status = NGX_HTTP_LIMIT_CONN_REJECTED;

                return lccf->status_code;
            }

            /*
             * NOTE: conn 是 u_short 类型，最大为 2^16
             */
            lc->conn++;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit conn: %08Xi %d", node->key, lc->conn);

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        /*
         * NOTE: 清理函数 ngx_http_limit_conn_cleanup 的作用是将连接数-1，
         *       并且在减至 0 时将对应的结点从红黑树中删除，并释放共享内存
         *
         * QUESTION: 这个函数什么时候被调调用呢？
         *           注意到这个 cleanup 是分配在 r->pool 这个请求维度的内存池的
         *           所以请求结束后内存池被释放，cleanup 就会被调用
         *
         * TODO: 这里让我对 limit_conn 和 limit_req 越来越分不清了
         *       看完 limit_conn 马上去看 limit_req
         */

        cln = ngx_pool_cleanup_add(r->pool,
                                   sizeof(ngx_http_limit_conn_cleanup_t));
        if (cln == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        cln->handler = ngx_http_limit_conn_cleanup;
        lccln = cln->data;

        lccln->shm_zone = limits[i].shm_zone;
        lccln->node = node;
    }

    return NGX_DECLINED;
}


static void
ngx_http_limit_conn_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p;
    ngx_http_limit_conn_node_t   *lcn, *lcnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lcn = (ngx_http_limit_conn_node_t *) &node->color;
            lcnt = (ngx_http_limit_conn_node_t *) &temp->color;

            p = (ngx_memn2cmp(lcn->data, lcnt->data, lcn->len, lcnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_rbtree_node_t *
ngx_http_limit_conn_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key, uint32_t hash)
{
    ngx_int_t                    rc;
    ngx_rbtree_node_t           *node, *sentinel;
    ngx_http_limit_conn_node_t  *lcn;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lcn = (ngx_http_limit_conn_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lcn->data, key->len, (size_t) lcn->len);

        if (rc == 0) {
            return node;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


static void
ngx_http_limit_conn_cleanup(void *data)
{
    ngx_http_limit_conn_cleanup_t  *lccln = data;

    ngx_rbtree_node_t           *node;
    ngx_http_limit_conn_ctx_t   *ctx;
    ngx_http_limit_conn_node_t  *lc;

    ctx = lccln->shm_zone->data;
    node = lccln->node;
    lc = (ngx_http_limit_conn_node_t *) &node->color;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, lccln->shm_zone->shm.log, 0,
                   "limit conn cleanup: %08Xi %d", node->key, lc->conn);

    lc->conn--;

    if (lc->conn == 0) {
        ngx_rbtree_delete(&ctx->sh->rbtree, node);
        ngx_slab_free_locked(ctx->shpool, node);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);
}


static ngx_inline void
ngx_http_limit_conn_cleanup_all(ngx_pool_t *pool)
{
    ngx_pool_cleanup_t  *cln;

    cln = pool->cleanup;

    while (cln && cln->handler == ngx_http_limit_conn_cleanup) {
        ngx_http_limit_conn_cleanup(cln->data);
        cln = cln->next;
    }

    pool->cleanup = cln;
}


/*
 * NOTE: init 函数在 ngx_init_cycle 中初始化共享内存时调用
 *       在 ngx_http_limit_conn_zone 函数中设置了 zone->init 和 zone->data
 */
static ngx_int_t
ngx_http_limit_conn_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_limit_conn_ctx_t  *octx = data;

    size_t                      len;
    ngx_http_limit_conn_ctx_t  *ctx;

    /*
     * NOTE: init zone 的时候需要考虑到 reload/update nginx 的情况，所以会把 old_cycle
     *       对应的(TODO: 这么个对应法)，一起来 init
     */
    ctx = shm_zone->data;

    if (octx) {
        if (ctx->key.value.len != octx->key.value.len
            || ngx_strncmp(ctx->key.value.data, octx->key.value.data,
                           ctx->key.value.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "limit_conn_zone \"%V\" uses the \"%V\" key "
                          "while previously it used the \"%V\" key",
                          &shm_zone->shm.name, &ctx->key.value,
                          &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    // QUESTION: 这是什么情况？
    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_http_limit_conn_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    // balus: 各种指针让我困惑...
    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_limit_conn_rbtree_insert_value);

    // QUESTION: 下面的字符串是用来干啥的？
    len = sizeof(" in limit_conn_zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in limit_conn_zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    if (r->main->limit_conn_status == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->len = ngx_http_limit_conn_status[r->main->limit_conn_status - 1].len;
    v->data = ngx_http_limit_conn_status[r->main->limit_conn_status - 1].data;

    return NGX_OK;
}


static void *
ngx_http_limit_conn_create_conf(ngx_conf_t *cf)
{
    ngx_http_limit_conn_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_conn_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    conf->log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->dry_run = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_limit_conn_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_conn_conf_t *prev = parent;
    ngx_http_limit_conn_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->log_level, prev->log_level, NGX_LOG_ERR);
    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    ngx_conf_merge_value(conf->dry_run, prev->dry_run, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                            *p;
    ssize_t                            size;
    ngx_str_t                         *value, name, s;
    ngx_uint_t                         i;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_limit_conn_ctx_t         *ctx;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_limit_conn_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &ctx->key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    size = 0;
    name.len = 0;

    /*
     * QUESTION: 为什么要用一个 for 循环，不是就最后一个参数是 zone=name:size 的格式么？
     */
    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            // NOTE: strstr 函数找到 ":" 在 data 中第一次出现的位置
            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            // NOTE: 最小也得 8*4 = 32KB
            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    /*
     * NOTE: 这里为 limit_conn_zone 指令分配共享内存
     *       如果 shm_zone->data 已经存在，说明已经有一个同名的 limit_conn_zone 了
     *       而这是不允许的
     */
    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to key \"%V\"",
                           &cmd->name, &name, &ctx->key.value);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_limit_conn_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


/*
 * NOTE: 多个 limit_conn 可以共存
 *
 * http {
 *     limit_conn_zone  $binary_remote_addr zone=addr:10m;
 *     limit_conn_zone  $server_name zone=servers:10m;

 *
 *     server {
 *         listen         9877;
 *         server_name    localhost;
 *
 *         root           /media/FLV;
 *         location ~\.flv {
 *             flv;
 *
 *             limit_conn   addr 1024;
 *             limit_conn   servers 64;
 *         }
 *     }
 * }
 *
 * 上面这段 conf 就在同一个 location 下配置了俩 limit_conn
 */

static char *
ngx_http_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_shm_zone_t               *shm_zone;
    ngx_http_limit_conn_conf_t   *lccf = conf;
    ngx_http_limit_conn_limit_t  *limit, *limits;

    ngx_str_t  *value;
    ngx_int_t   n;
    ngx_uint_t  i;

    value = cf->args->elts;

    // QUESTION: 为什么以 0 为 size？
    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * NOTE: 这里有个问题，如果我没有配置 limit_conn_zone，而直接使用了 limit_conn
     *       这样的配置文件也是没有问题的。
     *       那么就会造成产生 size 为 0 的 zone，这个在 ngx_init_cycle 的时候会报错
     *       是不是可以加上下面这段代码：
     *
     *  if (shm_zone->shm.size == 0) {
     *      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
     *                         "no zone named \"%V\"", &value[1]);
     *      return NGX_CONF_ERROR;
     *  }
     *
     * NOTE: 或者把不要用 ngx_shared_memory_add
     *
     *  ATTENTION: 上面这段代码是我写的，不是不小心被注释了
     */

    limits = lccf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lccf->limits, cf->pool, 1,
                           sizeof(ngx_http_limit_conn_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    /*
     * NOTE: 虽然像上面说的，多个 limit_conn 可以共存，但是必须是不同的 limit_conn_zone
     *
     * location ~\.flv {
     *     limit_conn  addr 1024;
     *     limit_conn  addr 2048;
     * }
     *
     * GUESS: 或许上面的 ngx_shared_memory_add 就是为了找到该 name 对应的 zone 的地址
     *        然后下面通过地址可以判断是否多个 limit_conn 用了一个 zone
     *        然后把 size 设置为 0 就是为了消除 size 的影响？
     *        还是说这里根本不想分配内存？但是这样后面怎么在 shm_zone 里面分配内存呢？
     *
     * TODO: 还没有细看 ngx_shared_memory_add 函数
     */
    for (i = 0; i < lccf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }

    n = ngx_atoi(value[2].data, value[2].len);
    if (n <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number of connections \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    // NOTE: u_short 类型
    if (n > 65535) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "connection limit must be less 65536");
        return NGX_CONF_ERROR;
    }

    /*
     *
     */
    limit = ngx_array_push(&lccf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

    limit->conn = n;
    limit->shm_zone = shm_zone;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_limit_conn_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_limit_conn_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


/*
 * NOTE: 这个函数决定本模块在哪个阶段生效
 *
 * QUESTION: 但是为什么用 postconfiguration 来做呢？
 */
static ngx_int_t
ngx_http_limit_conn_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_conn_handler;

    return NGX_OK;
}
