# Nginx-http请求的多阶段处理(一)

为什么 Nginx 要把请求的处理过程分为多个阶段呢？

Nginx 的模块化设计使得每一个 HTTP 模块可以仅专注于完成一个独立的、简单的功能，而一个请求的完整处理过程可以由无数个 HTTP 模块共同合作完成。

这种设计具有非常好的简单性、可测试性、可扩展性，然而，当多个 HTTP 模块流水式地处理同一个请求时，单一的处理顺序是无法满足灵活性需求的，每一个正在处理请求的 HTTP 模块很难灵活、有效地指定下一个 HTTP 处理模块是哪一个。而且，不划分处理阶段也会让 HTTP 请求的完整流程难以管理，每一个 HTTP 模块也很难正确地将自己插入到完整流程中的合适位置中。

## HTTP 请求的 11 个处理阶段

Nginx 将 HTTP 请求将要经历的阶段划分为 11 个：

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

## Nginx 中用来表示阶段的数据结构

所谓 Nginx 请求经历一个阶段，指的是让该阶段内的 HTTP 模块流水式地处理该请求。阶段的概念是由 Nginx 自己来划分的，但是阶段的内容却是由 HTTP 模块编写者来决定的。

我们可以想象，http 请求的处理流程被分为 11 个阶段，而每个阶段都由 http 模块注册的回调函数组成，所以这 11 个阶段应该是用一个二维数组来表示的，事实上也的确是这样：

```c
typedef struct {
    ...
    ngx_http_phase_t           phases[NGX_HTTP_LOG_PHASE + 1];
} ngx_http_core_main_conf_t;

typedef struct {
    ngx_array_t                handlers;
} ngx_http_phase_t;
```

我们写模块时，通常会像下面这样往其中的某个（或者几个）阶段中添加处理方法，比如
这里就往`NGX_HTTP_ACCESS_PHASE`中添加了`ngx_http_allow_in_handler`方法用以控制
访问:

```C
static ngx_int_t
ngx_http_allow_in_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_allow_in_handler;

    return NGX_OK;
}
```

## 二维结构到一维结构

上面描述了 phase 的二维数组结构，这个结构在 HTTP 模块中注册自己的 handler 时很方便，但是对于 nginx 执行来说则显得有些麻烦：

在`ngx_http_request_t` 结构中有一个字段：

```c
struct ngx_http_request_s {
    ...
    ngx_int_t                         phase_handler;
    ...
};
```

`phase_handler`字段指定了请求现在要接受的处理方法在`handlers`数组(`cmcf->phase_engine.handlers`)中的下标。是一个一维的情况，只能表示一维的。但是这还不是重点，毕竟为了适应二维我们完全可以这样：

```c
struct ngx_http_request_s {
    ...
    ngx_int_t                         phase_index;
    ngx_int_t                         handler_index;
    ...
};
```

虽然浪费点内存，但是也还可以接受。

重点在于我们经常需要一中**继续执行下一个处理方法**的情况，比如经常会这样：

```c
ngx_int_t
ngx_http_core_generic_phase(ngx_http_request_t *r, ngx_http_phase_handler_t *ph)
{
    rc = ph->handler(r);
    ...

    if (rc == NGX_DECLINED) {
        r->phase_handler++;
        return NGX_AGAIN;
    }

    ...
}
```

上面`r->phase_handler++`意思就是执行下一个处理方法，而不管它是属于本阶段还是下一个阶段，如果采用二维的情况，则需要进行比较多的逻辑判断。

所以在解析完所有的 http 配置项之后，nginx 会将这个二维数组给转换为一个一维结构。在全局的`ngx_http_core_main_conf_t`结构体中有一个`phase_engine`字段，里面就存储着所有阶段的内容：

```C
typedef struct {
    ...
    ngx_http_phase_engine_t    phase_engine;
    ...
    ngx_http_phase_t           phases[NGX_HTTP_LOG_PHASE + 1];
} ngx_http_core_main_conf_t;

typedef struct {
    ngx_http_phase_handler_t  *handlers;
    ngx_uint_t                 server_rewrite_index;
    ngx_uint_t                 location_rewrite_index;
} ngx_http_phase_engine_t;
```

`ngx_http_phase_engine_t`中有一个数组`handlers`，它就存储着所有 HTTP 模块的添加的处理方法，每个处理方法都是`ngx_http_phase_handler_t`类型：

```c
typedef struct ngx_http_phase_handler_s  ngx_http_phase_handler_t;

struct ngx_http_phase_handler_s {
    ngx_http_phase_handler_pt  checker;
    ngx_http_handler_pt        handler;
    ngx_uint_t                 next;
};
```

所以可以看到其实每一个 phase 就是一些 handler 的集合，这些 handler 是由 HTTP 模块自己插入进去的，这里有可以看到`ngx_http_phase_handler_t`中既有 handler，又有 checker 字段，这两者的角色需要区分：

* `checker`: 由 HTTP 框架提供，在 checker 方法中会调用 handler 方法，并且根据其 handler 的返回值来决定之后的执行流程
* `handler`: 真正由 HTTP 框架添加的处理方法，各 HTTP 模块就是通过这个来介入请求的处理流程的。
* `next`: 指向下一阶段的第一个 handler 的下标(在`cmcf->phase_engine.handlers`数组中的下标)。通过这个下标，我们可以不必完全按照流水线方式顺序执行完某个阶段内所有处理方法之后再顺序执行下一个阶段中的所有处理方法，而可以直接跳到下一个阶段去，而不管当前阶段内是否有其他尚未执行的处理方法。

### 如何进行所有阶段的初始化

阶段的初始化工作是在 ngx_http.c 文件中的`ngx_http_init_phase_handlers`函数中完成的：

```c
static ngx_int_t
ngx_http_init_phase_handlers(ngx_conf_t *cf, ngx_http_core_main_conf_t *cmcf)
{
    cmcf->phase_engine.server_rewrite_index = (ngx_uint_t) -1;
    cmcf->phase_engine.location_rewrite_index = (ngx_uint_t) -1;
    find_config_index = 0;
    use_rewrite = cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers.nelts ? 1 : 0;
    use_access = cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers.nelts ? 1 : 0;

    n = 1                  /* find config phase */
        + use_rewrite      /* post rewrite phase */
        + use_access;      /* post access phase */

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
        n += cmcf->phases[i].handlers.nelts;
    }

    ph = ngx_pcalloc(cf->pool,
                     n * sizeof(ngx_http_phase_handler_t) + sizeof(void *));
    if (ph == NULL) {
        return NGX_ERROR;
    }
```

首先统计所有阶段中的处理方法的个数。其中`n = 1 + use_rewrite + use_access`这句需要注意。从注释中也可以卡出它统计的是`NGX_HTTP_FIND_CONFIG`、`NGX_HTTP_POST_REWRITE_PHASE`和`NGX_HTTP_POST_ACCESS_PHASE`这三个阶段中处理方法的个数。这三个阶段是比较特殊的：

* `NGX_HTTP_FIND_CONFIG_PHASE`阶段是必不可少的，这个阶段是不能跳过的，而且任何
HTTP 模块都不可以往这一阶段中添加处理方法。所以直接`n + 1`
* `NGX_HTTP_POST_REWRITE_PHASE`阶段和`NGX_HTTP_FIND_CONFIG_PHASE`阶段一样只能由 HTTP 框架实现呢，而不允许 HTTP 模块往该阶段添加处理方法。这个阶段的意义在于检查 rewrite 重写的次数不超过 10 次。所以他的值是由`NGX_HTTP_REWRITE_PHASE`阶段来决定的，如果`NGX_HTTP_REWRITE_PHASE`阶段没有处理方法，也就不需要`NGX_HTTP_POST_REWRITE_PHASE`了。
* `NGX_HTTP_POST_ACCESS_PHASE`和`NGX_HTTP_POST_REWRITE_PHASE`阶段类似。也是一个只能由 HTTP 框架实现的阶段，而不允许 HTTP 模块向其中添加处理方法。这个阶段的作用是`ngx_http_request_t`结构中的`access_code`成员，如果其值不为 0，则结束请求(表示没有访问权限)，否则执行下一个处理方法。所以如果没有`NGX_HTTP_ACCESS_PHASE`阶段，`NGX_HTTP_POST_ACCESS_PHASE`阶段也就没有存在的必要了。

然后一个`for`循环统计每个阶段中的处理方法的个数。需要注意的是，这些处理方法都是HTTP 模块添加的，而不是 HTTP 框架预置的。比如前面提到`NGX_HTTP_FIND_CONFIG_PHASE`阶段的处理方法只能由 HTTP 框架指定，所以就不包含在`cmcf->phases[i].handlers.elts`中了，也就不会重复统计了。

```c
    cmcf->phase_engine.handlers = ph;
    n = 0;

    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {
        h = cmcf->phases[i].handlers.elts;

        switch (i) {
        case NGX_HTTP_SERVER_REWRITE_PHASE:
            if (cmcf->phase_engine.server_rewrite_index == (ngx_uint_t) -1) {
                cmcf->phase_engine.server_rewrite_index = n;
            }
            checker = ngx_http_core_rewrite_phase;

            break;

        case NGX_HTTP_FIND_CONFIG_PHASE:
            find_config_index = n;

            ph->checker = ngx_http_core_find_config_phase;
            n++;
            ph++;

            continue;

        case NGX_HTTP_REWRITE_PHASE:
            if (cmcf->phase_engine.location_rewrite_index == (ngx_uint_t) -1) {
                cmcf->phase_engine.location_rewrite_index = n;
            }
            checker = ngx_http_core_rewrite_phase;

            break;

        case NGX_HTTP_POST_REWRITE_PHASE:
            if (use_rewrite) {
                ph->checker = ngx_http_core_post_rewrite_phase;
                ph->next = find_config_index;
                n++;
                ph++;
            }

            continue;

        case NGX_HTTP_ACCESS_PHASE:
            checker = ngx_http_core_access_phase;
            n++;
            break;

        case NGX_HTTP_POST_ACCESS_PHASE:
            if (use_access) {
                ph->checker = ngx_http_core_post_access_phase;
                ph->next = n;
                ph++;
            }

            continue;

        case NGX_HTTP_CONTENT_PHASE:
            checker = ngx_http_core_content_phase;
            break;

        default:
            checker = ngx_http_core_generic_phase;
        }

        n += cmcf->phases[i].handlers.nelts;

        for (j = cmcf->phases[i].handlers.nelts - 1; j >= 0; j--) {
            ph->checker = checker;
            ph->handler = h[j];
            ph->next = n;
            ph++;
        }
    }
}
```

从`switch-case`中也可以看出来，`NGX_HTTP_FIND_CONFIG_PHASE`、`NGX_HTTP_POST_REWRITE_PHASE`以及`NGX_PHTT_POST_ACCESS_PHASE`这 3 个阶段的 checker 方法都是由 HTTP 框架指定的，而不再`cmcf->phases[i].handlers`中，所以直接`continue`而不是`break`，这样就不会执行`switch`之后的`for`循环了。

TODO: 这里比较难理解的是`NGX_HTTP_ACCESS_PHASE`阶段的处理方法，在该`case`中进行
了`n++`，然后 break 出 switch 语句之后执行其后的 for 循环，那不就重复计数了吗？
为什么要这么做呢？我暂时还不明白。

## 总结
