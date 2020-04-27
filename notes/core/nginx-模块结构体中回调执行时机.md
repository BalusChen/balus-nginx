# `ngx_module_t`结构体中各个回调的执行时机和顺序

`ngx_module_t`结构体是 Nginx 为各个模块提供的一个统一接口，用以简化 Nginx 的管理。

## `ngx_module_t`结构体中的回调函数

Nginx 中典型的模块定义是这样的：

```c
ngx_module_t  ngx_thread_pool_module = {
    NGX_MODULE_V1,
    &ngx_thread_pool_module_ctx,           /* module context */
    ngx_thread_pool_commands,              /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_thread_pool_init_worker,           /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_thread_pool_exit_worker,           /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};
```

可以发现一共有`init_master`, `init_module`, `init_process`, `init_thread`,
`exit_thread`, `exit_process`, `exit_master`这七个回调。

其中`init_master`, `init_thread`和`exit_thread`暂时还没有被使用(至少官方模块里面是这样)，毕竟
Nginx 目前还是多进程架构，所以就先不考虑这三个回调了。

那么来看看其他四个回调。

### `init_module`

对`init_module`字段查看引用可以发现它在`ngx_init_modules`函数中被调用了，继续向
上查询可以得到这条调用链：

```c
main -> ngx_init_cycle -> ngx_init_modules -> init_module
```

在`main`函数中，调用`ngx_init_cycle`之前还没有进入 master/single 循环的，更不用
说 worker 进程了。

### `init_process`

而对于`init_process`函数，通过 CLion 的查看引用可以得到以下调用链(使用的是多进程
而不是单进程模式)：

```c
main -> ngx_master_process_cycle -> ngx_start_worker_processes -> ngx_worker_process_cycle
-> ngx_worker_process_init -> init_process
```

而在调用`ngx_master_process_cycle`进入 master 工作循环之前，我们需要`ngx_init_cycle`
初始化工作循环，而`init_module`正式在`ngx_init_cycle`中被调用的，所以我们可以发现
`init_process`是在`init_module`后调用的。

而且在`ngx_master_process_cycle`中会调用`ngx_start_worker_processes`产生 worker
进程，所以又可以知道`init_module`是在尚未进入工作循环(没有 master，更没有 worker)
时执行的，而`init_process`则是在刚刚生成的 worker 进程中执行的。

### `exit_process`

### `exit_master`

## 与配置项有关

很少有模块会实现`init_process`等回调，虽然 rtmp/upstream 模块实现了，但是在它们的
`init_process`回调中，都没有涉及到配置项，这是我比较关注的东西。我想知道在哪个阶
段我们可以读取配置项，`init_process`中可以吗？

这里摘了`ngx_thread_pool`模块注册的`init_process`方法：

```c
static ngx_int_t
ngx_thread_pool_init_worker(ngx_cycle_t *cycle)
{
    ngx_uint_t                i;
    ngx_thread_pool_t       **tpp;
    ngx_thread_pool_conf_t   *tcf;

    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    tcf = (ngx_thread_pool_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                  ngx_thread_pool_module);

    if (tcf == NULL) {
        return NGX_OK;
    }

    ngx_thread_pool_queue_init(&ngx_thread_pool_done);

    tpp = tcf->pools.elts;

    for (i = 0; i < tcf->pools.nelts; i++) {
        if (ngx_thread_pool_init(tpp[i], cycle->log, cycle->pool) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
```

可以发现里面通过`ngx_get_conf`函数获取到了`ngx_thread_pool_module`的配置项结构体。
这里就可以拿到配置项结构体了吗？这是我奇怪的地方，所以我决定一探究竟。

转到`ngx_init_cycle`函数中来:

```c
ngx_cycle_t
ngx_init_cycle(ngx_cycle_t *old_cycle) {
   ...
   
    cycle->conf_ctx = ngx_pcalloc(pool, ngx_max_module * sizeof(void *));
    if (cycle->conf_ctx == NULL) {
        ngx_destroy_pool(pool);
        return NULL;
    }
    
    ...

    for (i = 0; cycle->modules[i]; i++) {
        if (cycle->modules[i]->type != NGX_CORE_MODULE) {
            continue;
        }

        module = cycle->modules[i]->ctx;

        if (module->create_conf) {
            rv = module->create_conf(cycle);
            if (rv == NULL) {
                ngx_destroy_pool(pool);
                return NULL;
            }
            cycle->conf_ctx[cycle->modules[i]->index] = rv;
        }
    }
    
    ...
    
    conf->ctx = cycle->conf_ctx;
    if (ngx_conf_param(&conf) != NGX_CONF_OK) {
        environ = senv;
        ngx_destroy_cycle_pools(&conf);
        return NULL;
    }

    if (ngx_conf_parse(&conf, &cycle->conf_file) != NGX_CONF_OK) {
        environ = senv;
        ngx_destroy_cycle_pools(&conf);
        return NULL;
    }
    
    ...
    
    for (i = 0; cycle->modules[i]; i++) {
        if (cycle->modules[i]->type != NGX_CORE_MODULE) {
            continue;
        }

        module = cycle->modules[i]->ctx;

        if (module->init_conf) {
            if (module->init_conf(cycle,
                                  cycle->conf_ctx[cycle->modules[i]->index])
                == NGX_CONF_ERROR)
            {
                environ = senv;
                ngx_destroy_cycle_pools(&conf);
                return NULL;
            }
        }
    }
    
    ...
}
```

扫一扫这个函数就可以发现我们想要的东西，可以发现在`ngx_init_cycle`函数中处理的是
`NGX_CORE_MODULE`类型的模块：

```c
typedef struct {
    ngx_str_t             name;
    void               *(*create_conf)(ngx_cycle_t *cycle);
    char               *(*init_conf)(ngx_cycle_t *cycle, void *conf);
} ngx_core_module_t;
```

首先当然是`create_conf`，然后是`parse_conf`，最后是`init_conf`。

TODO: 但是这里有一个问题就是，`parse_conf`究竟是解析哪一部分的配置项呢？

## 总结

里面比较难以区分的是`init_module`和`init_process`这两个回调。

## 参考

[关于 Nginx 的`init_module`和`init_process`接口](https://www.tuicool.com/articles/2U3y6b6)
