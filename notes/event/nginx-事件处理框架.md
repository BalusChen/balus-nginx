# NGINX 事件处理框架

事件处理框架所要解决的问题是如何收集、管理和分发事件。

这里所说的事件，主要有两类：

* 网络事件
* 定时器事件

## 网络事件

首先来看看 NGINX 是如何收集和管理 TCP 网络事件的。

由于网络事件与网卡中断程序、内核提供的系统调用密切相关，所以网络事件的驱动既取决于不同的操作系统平台，又在同一个操作系统平台中受制于不同的内核版本。比如 2.6 版本之前的 Linux 基本都是使用`select`和`poll`，但是现在的 Linux 则广泛使用`epoll`。而在现在的 FreeBSD 则使用的是`kqueue`

如此一来，事件处理框架就需要在不同操作系统内核中选择一种事件驱动机制支持网络事件的处理。NGINX 是怎么做到这一点的呢？

1. 首先定义一个核心模块`ngx_events_module`。

这个模块感兴趣的配置项只有`events {}`，NGINX 启动时会调用`ngx_init_cycle`方法来解析配置项，当找到所感兴趣的配置项时，这个模块就开始工作了。

`ngx_events_module`定义了事件类型的模块，它的全部工作就是为所有的事件模块解析`events {}`中的配置项，同时管理这些事件模块存储配置项的结构体。

2. 其次定义了一个很重要的事件模块`ngx_event_core_module`

这个模块会决定使用哪种事件驱动机制，以及如何管理事件。

3. 最后，NGINX 为不同操作系统以及不同内核版本定义了不同的事件驱动模块。比如`ngx_epoll_module`, `ngx_select_module`, `ngx_kqueue_module`等。

在`ngx_event_core_module`模块的初始化过程中，将会从这些事件驱动模块中选取 1 个作为 NGINX 进程的事件驱动模块。

### NGINX 事件的定义

```c
struct ngx_event_s {
    void            *data;

    unsigned         write:1;

    unsigned         accept:1;

    /* used to detect the stale events in kqueue and epoll */
    unsigned         instance:1;

    /*
     * the event was passed or would be passed to a kernel;
     * in aio mode - operation was posted.
     */
    unsigned         active:1;

    unsigned         disabled:1;

    /* the ready event; in aio mode 0 means that no operation can be posted */
    unsigned         ready:1;

    unsigned         oneshot:1;

    /* aio operation is complete */
    unsigned         complete:1;

    unsigned         eof:1;
    unsigned         error:1;

    unsigned         timedout:1;
    unsigned         timer_set:1;

    unsigned         delayed:1;

    unsigned         deferred_accept:1;

    /* the pending eof reported by kqueue, epoll or in aio chain operation */
    unsigned         pending_eof:1;

    unsigned         posted:1;

    unsigned         closed:1;

    /* to test on worker exit */
    unsigned         channel:1;
    unsigned         resolver:1;

    unsigned         cancelable:1;

#if (NGX_HAVE_KQUEUE)
    unsigned         kq_vnode:1;

    /* the pending errno reported by kqueue */
    int              kq_errno;
#endif

    /*
     * kqueue only:
     *   accept:     number of sockets that wait to be accepted
     *   read:       bytes to read when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *   write:      available space in buffer when event is ready
     *               or lowat when event is set with NGX_LOWAT_EVENT flag
     *
     * epoll with EPOLLRDHUP:
     *   accept:     1 if accept many, 0 otherwise
     *   read:       1 if there can be data to read, 0 otherwise
     *
     * iocp: TODO
     *
     * otherwise:
     *   accept:     1 if accept many, 0 otherwise
     */

#if (NGX_HAVE_KQUEUE) || (NGX_HAVE_IOCP)
    int              available;
#else
    unsigned         available:1;
#endif

    ngx_event_handler_pt  handler;


#if (NGX_HAVE_IOCP)
    ngx_event_ovlp_t ovlp;
#endif

    ngx_uint_t       index;

    ngx_log_t       *log;

    ngx_rbtree_node_t   timer;

    /* the posted queue */
    ngx_queue_t      queue;

#if 0

    /* the threads support */

    /*
     * the event thread context, we store it here
     * if $(CC) does not understand __thread declaration
     * and pthread_getspecific() is too costly
     */

    void            *thr_ctx;

#if (NGX_EVENT_T_PADDING)

    /* event should not cross cache line in SMP */

    uint32_t         padding[NGX_EVENT_T_PADDING];
#endif
#endif
};

```

* `data`: 通常指向该时间的`ngx_connection_t`连接对象
* `accept`: 表示
* `instance`: 区分事件是否过期
* `active`: 指事件被添加到epoll对象的监控中
* `ready`: 表示被监控的事件已经准备就绪，即可以对其进程IO处理

### 如何操作事件

事件是不需要创建的，NGINX 默认每个连接对应一个读事件和写事件，NGINX 在启动时会创建连接池(`ngx_cycle_t::connections`)，而且为每个连接都会分配一个读事件(`ngx_cycle_t::read_events`)和一个写事件(`ngx_cycle_t::write_events`)。

如何将事件添加到`epoll`等事件驱动模块中去呢？

我们知道`ngx_module_t`表示 NGINX 模块的通用接口，而针对每一种不同类型的模块，都有一个结构体来描述**这一类模块的通用接口**，这个接口保存在`ngx_module_t`的`ctx`字段中(`void *ctx`)。对于事件模块，其通用接口为`ngx_event_module_t`:

```c
typedef struct {
    ngx_str_t              *name;

    void                 *(*create_conf)(ngx_cycle_t *cycle);
    char                 *(*init_conf)(ngx_cycle_t *cycle, void *conf);

    ngx_event_actions_t     actions;
} ngx_event_module_t;
```

* `create_conf`: 这个回调方法用于创建存储配置项参数的结构体
* `init_conf`: 在解析完成之后，这个回调方法会被调用，用以综合处理当前事件模块感兴趣的全部配置项

还有一个`actions`成员，这个是定义事件驱动模块的核心方法：

```c
typedef struct {
    ngx_int_t  (*add)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*del)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*enable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);
    ngx_int_t  (*disable)(ngx_event_t *ev, ngx_int_t event, ngx_uint_t flags);

    ngx_int_t  (*add_conn)(ngx_connection_t *c);
    ngx_int_t  (*del_conn)(ngx_connection_t *c, ngx_uint_t flags);

    ngx_int_t  (*notify)(ngx_event_handler_pt handler);

    ngx_int_t  (*process_events)(ngx_cycle_t *cycle, ngx_msec_t timer,
                                 ngx_uint_t flags);

    ngx_int_t  (*init)(ngx_cycle_t *cycle, ngx_msec_t timer);
    void       (*done)(ngx_cycle_t *cycle);
} ngx_event_actions_t;
```

* `add`: 负责把一个感兴趣的时间添加到事件驱动机制中去。这样，在事件发生后，可以调用`process_events`获取这个事件
* `del`: 负责把一个已经存在于事件驱动机制中的事件移除，这样以后即使这个事件发生了，调用`process_events`也无法再获取到这个事件
* `add_conn`: 向事件驱动机制中添加一个新的连接，这意味着该连接上的读写事件都添加到事件驱动机制中了
* `del_conn`: 从事件驱动机制中移除一个连接的读写事件
* `process_events`: 这个方法是处理、分发事件的核心
* `disable`和`enable`: 目前大都设置为和`add`, `del`一致
* `notify`: 仅在多线程环境下会被调用
* `init`: 初始化事件模块的方法
* `done`: 退出事件驱动模块前调用的方法

所以我们当然可以使用`ngx_event_actions_t`中的`add`和`del`方法来将事件加入到事件驱动机制或者将事件从事件驱动机制中移除。
但是 NGINX 也为我们提供了`ngx_handle_read_event`和`ngx_handle_write_event`这两个通用性方法。

```c
ngx_int_t
ngx_handle_read_event(ngx_event_t *rev, ngx_int_t event, ngx_uint_t flags)
{
    ...
}
```

* `rev`: 要操作的事件
* `flags`: 会指定事件的驱动方式，对于不同的事件驱动模块，`flags`的取值也会不同。对于`epoll`，`flags`可以取值`0`或者`NGX_CLOSE_EVENT`(这个只在`epoll`的 LT 水平触发模式下才有效，而 NGINX 主要工作在 ET 边缘触发模式下，所以一般可以忽略`flags`这个参数)

```c
ngx_int_t
ngx_handle_write_event(ngx_event_t *wev, size_t lowat)
{
    ...
}
```

* `lowat`: 表示只有当连接对应的套接字缓冲区中至少有`lowat`大小的可用空间时，时间收集器(`epoll`, `select`等)才能处理这个可写事件(`lowat` == 0时表示不考虑可写缓冲区大小)

## `ngx_events_module`核心模块

`ngx_events_module`模块是一个核心模块，类型为`NGX_CORE_MODULE`。它定义了一类新模块：事件模块。它主要做的事情有：

* 定义新的事件类型，并定义每个事件模块都必须实现的`ngx_event_module_t`接口
* 管理这些事件模块生成的配置项结构体，并解析事件配置项

```c
static ngx_command_t  ngx_events_commands[] = {

    { ngx_string("events"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_events_block,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_events_module_ctx = {
    ngx_string("events"),
    NULL,
    ngx_event_init_conf
};


ngx_module_t  ngx_events_module = {
    NGX_MODULE_V1,
    &ngx_events_module_ctx,                /* module context */
    ngx_events_commands,                   /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};
```

在`ngx_events_module_ctx`中也可以看到，它只定义了`init_conf`回调函数，而没有定义`create_conf`回调函数。这是因为`ngx_events_module`并不会解析配置项的参数，只是在出现了`events {}`配置项后会调用各事件模块去解析`events {...}`块内的配置项，所以就不需要实现`create_conf`方法了；那么为什么要实现`init_conf`方法呢？// TODO //

在`ngx_events_commands`数组中也可以发现，当在配置文件中发现这个`events {}`配置项时，就会调用`ngx_events_block`回调函数对该配置项进行解析，配置项结构体指针的保存也就是在`ngx_events_block`函数中完成的。


### 如何管理所有事件模块的配置项

#### 配置项结构体的存储

已经知道每一个事件模块都必须实现`ngx_event_module_t`接口

```c
typedef struct {
    ngx_str_t              *name;

    void                 *(*create_conf)(ngx_cycle_t *cycle);
    char                 *(*init_conf)(ngx_cycle_t *cycle, void *conf);

    ngx_event_actions_t     actions;
} ngx_event_module_t;
```

这个接口中允许每个模块建立自己的配置项结构体。其中的`create_conf`方法就是用来创建这个结构体的，事件模块只需要在这个方法中分配内存即可，而每一个事件模块产生的配置项结构体指针又会被放到`ngx_events_module`模块创建的指针数组中去，然后这个指针数组又会被放到`ngx_cycle_t`的`conf_ctx`字段中去。

```c
struct ngx_cycle_s {
    void            ****conf_ctx;
    ...
};
```

很可怕的四级指针！首先它是一个数组，这个数组中的每个元素都是一个三级指针：

```c
/* TODO */
```

每一个事件模块是如何获取它在`create_conf`中分配的结构体的指针呢：

``` C
#define ngx_event_get_conf(conf_ctx, module)                                  \
             (*(ngx_get_conf(conf_ctx, ngx_events_module))) [module.ctx_index]

```

还是挺难懂的，首先看看`ngx_get_conf`这个宏:

```c
#define ngx_get_conf(conf_ctx, module)  conf_ctx[module.index]
```

这里用到了`index`和`ctx_index`两个字段：

```c
struct ngx_module_s {
    ngx_uint_t          ctx_index;
    ngx_uint_t          index;

    ...
};
```

我们知道在 NGINX 中各模块在`ngx_modules`数组中的顺序是非常重要的，依靠`index`字段，每个模块才可以把自己的位置和其他模块的位置相比较，并以此决定行为。但是 NGINX 同时又允许定义子类型，比如说事件类型，HTTP 类型，邮件类型。区分同一类型中的模块当然也可以使用`index`字段(毕竟他是所有模块在`ngx_modules`数组中的位置)，但是这样效率太差。这时候`ctx_index`字段就派上用场了，`ctx_index`表明了模块在相同类型模块中的顺序。

从下面这张图可以看出`conf_ctx`是如何组织的：

![`conf_ctx`](../images/nginx-conf_ctx.png)

#### 配置项的管理

前面已经知道，`ngx_events_module` 感兴趣的配置项只有`events {}`:

```c
static char *
ngx_events_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                 *rv;
    void               ***ctx;
    ngx_uint_t            i;
    ngx_conf_t            pcf;
    ngx_event_module_t   *m;
```

那么这个函数做了哪些事情呢？

![`ngx_events_module`](../images/nginx-ngx_events_module-1.png)

结合源码看它的实现：

1. 初始化所有事件模块的`ctx_index`

```c
    if (*(void **) conf) {
        return "is duplicate";
    }

    /* count the number of the event modules and set up their indices */

    ngx_event_max_module = ngx_count_modules(cf->cycle, NGX_EVENT_MODULE);

```

主要的工作都在`ngx_count_modules`中做完了。但是有一点不太好理解的就是第一个`if`判断。

2. 分配指针数组，存储所有事件模块生成的配置项结构体指针

```c
    ctx = ngx_pcalloc(cf->pool, sizeof(void *));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *ctx = ngx_pcalloc(cf->pool, ngx_event_max_module * sizeof(void *));
    if (*ctx == NULL) {
        return NGX_CONF_ERROR;
    }
```

由于是多级指针，其实也不是那么好理解。首先`ctx`是一个三级指针，联想到`ngx_cycle_t`结构中`conf_ctx`字段是一个四级指针，所以可以猜到`ctx`应该是作为`conf_ctx`数组的一个成员，用以存储所有事件模块产生的配置项结构体的指针。

写了一个小程序以帮助理解：

```c
#define CCDESIZE    16

int
main(int argc, char **argv)
{
    int                i, *ip;
    void            ***ctx;

    ctx = malloc(sizeof(void *));
    if (ctx == NULL) {
        fprintf(stderr, "malloc() error\n");
        exit(-1);
    }

    *ctx = malloc(CCDESIZE * sizeof(void *));
    if (*ctx == NULL) {
        fprintf(stderr, "malloc() error");
        exit(-1);
    }

    for (i = 0; i < CCDESIZE; i++) {
        ip = malloc(sizeof(int));
        *ip = i * 7;
        (*ctx)[i] = ip;
    }

    for (int i = 0; i < CCDESIZE; i++) {
        ip = (*ctx)[i];
        printf("*(*ctx)[i] = %d\n", *ip);
    }

    return 0;
}
```

3. 调用所有事件模块的`create_conf`方法

```c
    *(void **) conf = ctx;

    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;

        if (m->create_conf) {
            (*ctx)[cf->cycle->modules[i]->ctx_index] =
                                                     m->create_conf(cf->cycle);
            if ((*ctx)[cf->cycle->modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }
```

其中有一个地方不懂的就是第一句`*(void **) conf = ctx`，`ctx`明明是一个三级指针，但是怎么放到作为二级指针(强制转换)的`conf`里面去了？TODO

4. 为所有事件模块解析`nginx.conf`配置文件

```c
    pcf = *cf;
    cf->ctx = ctx;
    cf->module_type = NGX_EVENT_MODULE; cf->cmd_type = NGX_EVENT_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = pcf;

    if (rv != NGX_CONF_OK) {
        return rv;
    }
```

看看`ngx_conf_t`结构：

```c
struct ngx_conf_s {
    char                 *name;
    ngx_array_t          *args;

    ngx_cycle_t          *cycle;
    ngx_pool_t           *pool;
    ngx_pool_t           *temp_pool;
    ngx_conf_file_t      *conf_file;
    ngx_log_t            *log;

    void                 *ctx;
    ngx_uint_t            module_type;
    ngx_uint_t            cmd_type;

    ngx_conf_handler_pt   handler;
    void                 *handler_conf;
};
```

5. 调用所有事件模块的`init_conf`方法

```c
    for (i = 0; cf->cycle->modules[i]; i++) {
        if (cf->cycle->modules[i]->type != NGX_EVENT_MODULE) {
            continue;
        }

        m = cf->cycle->modules[i]->ctx;

        if (m->init_conf) {
            rv = m->init_conf(cf->cycle,
                              (*ctx)[cf->cycle->modules[i]->ctx_index]);
            if (rv != NGX_CONF_OK) {
                return rv;
            }
        }
    }

    return NGX_CONF_OK;
}
```

可以看到，上面`create_conf`将分配的结构体指针存入了`(*ctx)[cf->cycle->modules[i]->ctx_index]`中，现在就在`init_conf`中用上了。

## `ngx_event_core_module` 模块

`ngx_event_core_module`是一个事件模块(而`ngx_events_module`是一个核心模块)，其类型为`NGX_EVENT_MODULE`。

它完成的任务主要有：

* 创建连接池(以及每个连接对应的读写事件)
* 决定使用那些事件驱动机制
* 初始化将要使用的事件模块

所以它在所有事件模块中的顺序是第一位的(`configure`时会自动摆放)，这样才可以保证它会先于其他事件模块执行，由此它选择事件驱动机制的任务才可以完成。

### `ngx_event_core_module`的定义

首先来看看它的定义：

```c
static ngx_str_t  event_core_name = ngx_string("event_core");


static ngx_command_t  ngx_event_core_commands[] = {

    { ngx_string("worker_connections"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_connections,
      0,
      0,
      NULL },

    { ngx_string("use"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_use,
      0,
      0,
      NULL },

    { ngx_string("multi_accept"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, multi_accept),
      NULL },

    { ngx_string("accept_mutex"),
      NGX_EVENT_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex),
      NULL },

    { ngx_string("accept_mutex_delay"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      0,
      offsetof(ngx_event_conf_t, accept_mutex_delay),
      NULL },

    { ngx_string("debug_connection"),
      NGX_EVENT_CONF|NGX_CONF_TAKE1,
      ngx_event_debug_connection,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_event_module_t  ngx_event_core_module_ctx = {
    &event_core_name,
    ngx_event_core_create_conf,            /* create configuration */
    ngx_event_core_init_conf,              /* init configuration */

    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};


ngx_module_t  ngx_event_core_module = {
    NGX_MODULE_V1,
    &ngx_event_core_module_ctx,            /* module context */
    ngx_event_core_commands,               /* module directives */
    NGX_EVENT_MODULE,                      /* module type */
    NULL,                                  /* init master */
    ngx_event_module_init,                 /* init module */
    ngx_event_process_init,                /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};
```

主要是看看它感兴趣的几个配置项：

* `worker_connections`: 连接池的大小(和`ngx_cycle_t::connections`有关)
* `connections`: 和`worker_connections`的意义相同
* `use`: 确定选择哪一个事件模块作为事件驱动机制，比如`use epoll`
* `multi_accept`: 在接收到一个新连接事件时，调用`accept`尽可能多地接受连接
* `accept_mutex`: 负载均衡锁
* `accept_mutex_delay`: 负载均衡锁会使有些 worker 进程拿不到锁时延迟建立新连接，这个选项就是这段延迟时间的长度
* `debug_connection`: 需要对来自指定 IP 的 TCP 连接打印 debug 级别的日志

另外，`ngx_event_core_module`只实现了`ngx_event_module_t`中的`create_conf`和`init_conf`方法，而没有实现`actions`接口，这是因为它并不真正负责 TCP 网络事件的驱动，所以不会实现`ngx_event_actions_t`中的方法。

在`ngx_module_t`结构中，`ngx_event_core_module`实现了`ngx_event_module_init`和`ngx_event_process_init`两个函数。

在 NGINX 启动过程中，还没有`fork`出子进程时，会首先调用`ngx_event_module_init`方法；然后在`fork()`出子进程之后，每一个 worker 进程会在调用`ngx_event_process_init`方法后再进入工作循环。

`ngx_event_module_init`：主要初始化了一些变量，而`ngx_event_process_init`方法就做了许多事情：

#### `ngx_event_process_init`函数

![`ngx_event_core_module`](../images/nginx-ngx_event_core_module.png)

这个函数很长(300多行)，所以就挑重点来看看：

```c
static ngx_int_t
ngx_event_process_init(ngx_cycle_t *cycle)
{
    ngx_uint_t           m, i;
    ngx_event_t         *rev, *wev;
    ngx_listening_t     *ls;
    ngx_connection_t    *c, *next, *old;
    ngx_core_conf_t     *ccf;
    ngx_event_conf_t    *ecf;
    ngx_event_module_t  *module;

```


1. 首先进行负载均衡锁的配置

```c
    if (ccf->master && ccf->worker_processes > 1 && ecf->accept_mutex) {
        ngx_use_accept_mutex = 1;
        ngx_accept_mutex_held = 0;
        ngx_accept_mutex_delay = ecf->accept_mutex_delay;

    } else {
        ngx_use_accept_mutex = 0;
    }
```

可以看到，当在配置文件中指定了使用负载均衡锁，并且是在 master 模式下，以及 worker 进程多于一个，才真正确定使用`accept_mutex`

2. 初始化定时器

```c
    ngx_queue_init(&ngx_posted_accept_events);
    ngx_queue_init(&ngx_posted_events);

    if (ngx_event_timer_init(cycle->log) == NGX_ERROR) {
        return NGX_ERROR;
    }
```

在 NGINX 中，定时器都被挂在一棵红黑树上。

3. 根据`use`配置项的参数，决定使用哪个事件驱动模块，并且调用`ngx_event_actions_t`中的`init`方法进行这个事件模块的初始化工作。

```c
    for (m = 0; cycle->modules[m]; m++) {
        if (cycle->modules[m]->type != NGX_EVENT_MODULE) {
            continue;
        }

        if (cycle->modules[m]->ctx_index != ecf->use) {
            continue;
        }

        module = cycle->modules[m]->ctx;

        if (module->actions.init(cycle, ngx_timer_resolution) != NGX_OK) {
            /* fatal */
            exit(2);
        }

        break;
    }
```

比如说在 nginx.conf 文件中指定`use epoll;`，那么首先在遇到`use`这个配置项的时候，就会调用`ngx_event_use`这个回调方法把配置项结构体中的`use`字段设置为 epoll 模块的`ctx_index`，现在 Nginx 就知道该用哪个事件驱动模块了。
在所有模块中找到`ngx_epoll_module`，然后使用这个模块实现的`actions`接口中的`init`函数进行这个模块的初始化工作。

```c
typedef struct {
    ...

    ngx_event_actions_t     actions;
} ngx_event_module_t;
```

可以看到，事件模块必须实现的`ngx_event_module`中有一个`ngx_event_actions_t`类型的`actions`接口：

```c
typedef struct {
    ...

    ngx_int_t  (*init)(ngx_cycle_t *cycle, ngx_msec_t timer);

    ...
} ngx_event_actions_t;
```

而`ngx_event_actions_t`中的`init`字段就是初始化事件模块的方法。

4. 初始化连接池和对应的读、写事件

```c
    cycle->connections =
        ngx_alloc(sizeof(ngx_connection_t) * cycle->connection_n, cycle->log);
    if (cycle->connections == NULL) {
        return NGX_ERROR;
    }

    c = cycle->connections;

    cycle->read_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                   cycle->log);
    if (cycle->read_events == NULL) {
        return NGX_ERROR;
    }

    rev = cycle->read_events;
    for (i = 0; i < cycle->connection_n; i++) {
        rev[i].closed = 1;
        rev[i].instance = 1;
    }

    cycle->write_events = ngx_alloc(sizeof(ngx_event_t) * cycle->connection_n,
                                    cycle->log);
    if (cycle->write_events == NULL) {
        return NGX_ERROR;
    }

    wev = cycle->write_events;
    for (i = 0; i < cycle->connection_n; i++) {
        wev[i].closed = 1;
    }

    i = cycle->connection_n;
    next = NULL;

    do {
        i--;

        c[i].data = next;
        c[i].read = &cycle->read_events[i];
        c[i].write = &cycle->write_events[i];
        c[i].fd = (ngx_socket_t) -1;

        next = &c[i];
    } while (i);

    /* balus:
     * at beginning, all connections are free connections
     */
    cycle->free_connections = next;
    cycle->free_connection_n = cycle->connection_n;
```

首先当然是为`connections`、`read_events`和`write_events`这三个数组分配内存了，然后需要将同一个下标的连接、读事件和写事件绑定在一起，而且由于一开始都是空闲连接，所以需要将所有连接都串在一起。对于空闲连接，它们复用`ngx_connection_t`结构中的`data`字段作为链表的 next 指针。

5. 对监听描述符进行操作

每个 worker 进程都监听着一些端口，这些相关的套接字描述符都存放在`ngx_cycle_t`中的`listening`数组中，这是一个`ngx_listening_t`结构的数组：

```c
struct ngx_listening_s {
    ngx_socket_t                fd;

    ...

    int                         type;

    ...

    ngx_connection_handler_pt   handler;

    unsigned                    reuseport:1;
    ...
    unsigned                    deferred_accept:1;
    ...
};
```

* `fd`: 监听描述符
* `type`: 套接字类型，比如为`SOCK_STREAM`时表示 TCP
* `handler`: 表示在这个监听端口上成功建立新的 TCP 连接后，就会回调 handler 方法。
* 

什么时候存的呢？(TODO: 我觉得应该是在解析配置文件的时候存的，但是解析配置文件分为好几个阶段，现在还不知道具体有哪些阶段，以后看到了相关代码再说吧)。

这一块要为处理**惊群**(thundering herd)问题做好准备，所以我感觉还是比较难理解的：

```c
    ls = cycle->listening.elts;
    for (i = 0; i < cycle->listening.nelts; i++) {

#if (NGX_HAVE_REUSEPORT)
        /*
         * 如果这个监听设置了 reuseport，
         * 而且该监听所在的 worker 进程并不是 ngx_worker(TODO：这里不太懂)
         *
         * 那么，说明这个套接字和其他进程中的套接字共享了端口，
         * 而且这个套接字上的事件已经被加入到了 epoll，所以就跳过不用处理
         */
        if (ls[i].reuseport && ls[i].worker != ngx_worker) {
            continue;
        }
#endif

        /*
         * 从连接池中取出一个新的连接
         */
        c = ngx_get_connection(ls[i].fd, cycle->log);

        if (c == NULL) {
            return NGX_ERROR;
        }

        c->type = ls[i].type;
        c->log = &ls[i].log;

        c->listening = &ls[i];
        ls[i].connection = c;

        rev = c->read;

        rev->log = c->log;
        rev->accept = 1;

#if (NGX_HAVE_DEFERRED_ACCEPT)
        /*
         * deferred_accept 表示并不是刚刚 accpt 就建立连接
         * 而是等到真正数据传送的时候再建立
         */
        rev->deferred_accept = ls[i].deferred_accept;
#endif

        /*
         *  如果不使用 IOCP 的话？
         *
         * TODO: 这里不太懂
         */
        if (!(ngx_event_flags & NGX_USE_IOCP_EVENT)) {
            if (ls[i].previous) {

                /*
                 * delete the old accept events that were bound to
                 * the old cycle read events array
                 */

                old = ls[i].previous->connection;

                if (ngx_del_event(old->read, NGX_READ_EVENT, NGX_CLOSE_EVENT)
                    == NGX_ERROR)
                {
                    return NGX_ERROR;
                }

                old->fd = (ngx_socket_t) -1;
            }
        }

        /*
         * 设置事件发生时的处理方法
         * 由于这里是监听套接字，所以如果是读事件发生了，那么说明有连接到来
         * 连接到来，如果是 TCP，则需要 accept，UDP 直接 recvmsg 即可
         *
         * TODO: 但是 NGINX 现在会用 UDP 么？
         */
        rev->handler = (c->type == SOCK_STREAM) ? ngx_event_accept
                                        : ngx_event_recvmsg;


        /*
         * 下面为了给解决惊群问题做准备，而需要从 REUSEPORT，EPOLLEXCLUSIVE和 accpt_mutex 锁中选取一种
         * 当然，如果 REUSEPORT，EPOLLEXCLUSIVE 不支持，而且 accept_mutex 没有打开的话
         * 就只能将事件直接加入到 epoll 中去了
         */
#if (NGX_HAVE_REUSEPORT)

        if (ls[i].reuseport) {
            if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
                return NGX_ERROR;
            }

            continue;
        }

#endif

		/* balus:
		 * why `continue` with accept_mutex used?
		 */
        if (ngx_use_accept_mutex) {
            continue;
        }

#if (NGX_HAVE_EPOLLEXCLUSIVE)

        /* balus:
         * use EPOLLEXCLUSIVE flag to solve thundering herd problem
         * in `epoll_wait`
         */
        if ((ngx_event_flags & NGX_USE_EPOLL_EVENT)
            && ccf->worker_processes > 1)
        {
            if (ngx_add_event(rev, NGX_READ_EVENT, NGX_EXCLUSIVE_EVENT)
                == NGX_ERROR)
            {
                return NGX_ERROR;
            }

            continue;
        }

#endif
        if (ngx_add_event(rev, NGX_READ_EVENT, 0) == NGX_ERROR) {
            return NGX_ERROR;
        }
```
