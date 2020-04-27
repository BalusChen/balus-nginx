# HTTP 模块中配置项的管理

nginx 中很重要的一部分就是配置项的管理，不仅用户自己的模块可以添加配置项，nginx 主干模块也定义了各种各样的配置项；
而且对于不同类型的模块，配置项的管理也不同，而其中最复杂的部分，就是 HTTP 模块的配置项管理了。
这里主要是要厘清 nginx 配置项是如何管理的，重点则在 HTTP 模块。

## 图表总结

之前一直看不懂 http 模块下的配置项是如何管理的，也看不懂总的管理流程，直到发现了下面这张图，果然是一图胜千言：

![ngx-conf-chart](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/ngx-conf-chart.png)

这个涉及到`ngx_cycle_t`结构中的四级指针：

```c
struct ngx_cycle_s
{
    ...
    void    ****conf_ctx;
    ...
};
```

总的来说，所有配置项结构体指针都被存储在`conf_ctx`这个四级指针中，
为什么是四级，这个主要是为 HTTP 模块考虑的，因为 HTTP 模块的配置项层次最深，所以得以它为标准。
而对于其他模块，比如说 event 模块，则不需要这么多层次，所以它作为`conf_ctx`数组中的一个成员，
本该是一个三级指针，却被当成二级指针来使用。其他模块甚至还有当成一级指针来使用的。
而对于 HTTP 模块，作为`conf_ctx`指针成员，则是实打实的一个三级指针。

## HTTP 模块配置项的管理

最复杂的就是 HTTP 模块配置项的管理了，复杂的原因是 http 块中还可以有 server 块，server 块中可以有 location 块，location 块又可以继续嵌套，而且同一配置项不仅仅可以出现在 server 块下，还可以同时出现在 http 和 location 块下，为了消除二义性并且扩大灵活度，nginx 还支持配置项的合并...诸多层次嵌套以及配置项的存在方式让配置项的管理难度骤然上升。为了方便管理，nginx 设计了许多精妙的数据结构，只有深刻理解了配置项的作用方法和这些数据结构的使用，才能真正明白为什么 nginx 这样管理 HTTP 模块的配置项。

nginx 目前一共有五大类型的模块：

* 核心模块
* 配置模块
* 事件模块
* HTTP 模块
* mail 模块

其中核心模块和配置模块是整个 nginx 所有模块的基础，它们和 nginx 框架密切相关。而对于事件模块、HTTP 模块和 mail 模块都不会与框架产生直接的关系，实际上他们在核心模块中各有一个模块作为自己的“代言人”，而在同种类模块中有一个作为核心业务与管理功能的模块。

比如说 HTTP 模块，它在核心模块中的代言人为 `ngx_http_module`，其类型为`NGX_CORE_MODULE`，而在 HTTP 类模块的内部，则有一个`ngx_http_core_module`作为核心业务与管理功能的模块，注意这个模块虽然带有"core"，但是实际上却只是一个 HTTP 模块，只不过它比一般的 HTTP 模块要特殊。

在 HTTP 模块中，最重要的就是这两个模块，`ngx_http_module`会负责加载所有的 HTTP 模块，但是业务的核心逻辑以及多具体的请求该选用哪一个 HTTP 模块处理，则是由`ngx_http_core_module`来决定的。

### 管理 main 级别下的配置项

### 管理 srv 级别下的配置项

在碰到 server{} 块时，就会调用`ngx_http_core_server`函数，

http{} 块可能存在着多个 server{} 块，怎么把这些 server{} 块给组织起来呢？
为什么要谈怎么把 server{} 块组织起来的问题呢？server{} 块下不是有一个`ngx_http_conf_t`实例么？该结构中不是已经有了`srv_conf`和`loc_conf`数组了吗？它们不是已经把出现在该 server{} 块中的 srv、loc 级别的配置项给组织起来啦吗？

的确是这样，但是在更高一层看，比如 http{} 块中想知道有其下所有 server{} 的情况，该怎么办呢？通过 http{} 块下的`ngx_http_conf_t`结构是不可能了，因为它没有指向下一级的指针。这就需要通过 http-core 模块中的 main 级别配置项来组织了：

```c
typedef struct {
    ngx_array_t                 servers;    /* ngx_http_core_srv_conf_t */
    ...
} ngx_http_core_main_conf_t;
```

其中有一个字段`servers`，是一个`ngx_http_core_srv_conf_t`的动态数组。
而每遇到一个 server{} 块，都会创建`ngx_http_conf_t`结构，其中`srv_conf`数组中第一个元素就是为 http-core 模块准备的，里面存储的是`ngx_http_core_srv_conf_t`的指针，而`ngx_http_core_srv_conf_t`结构中有一个`ctx`字段：

```c
typedef struct {
    ...
    ngx_http_conf_t         *ctx;
    ...
} ngx_http_core_srv_conf_t;
```

所以解析到一个 server{} 块时，会创建一个`ngx_http_conf_t`结构体，然后把该结构体指针放入该结构体中`srv_conf`数组的第一个元素(`ngx_http_core_srv_conf_t`)的`ctx`字段中，然后把这第一个元素放到**全局**的`ngx_http_core_main_conf_t`中的`servers`数组中。这就完成了 http{} 对 所有 server{} 的管理。

### 管理 loc 级别下的配置项

这里同样有怎么让 server{} 管理其下的所有 location{} 块的问题，除此之外，由于 location{} 块是可以嵌套的，所以又增加了新的问题，即让 location{} 块管理其下的子 location{} 块。

### 关于配置项的 merge 操作

首先呢得解释清楚什么叫做 main 级别，srv 级别，loc 级别，这得从`ngx_command_t`结构说起：

```c
struct ngx_command_s {
    ngx_str_t             name;
    ngx_uint_t            type;
    char               *(*set)(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
    ngx_uint_t            conf;
    ngx_uint_t            offset;
    void                 *post;
};
```

这里难以理解的是`type`和`conf`这两个字段。

`type`字段表示该配置项可以出现在哪些地方，比如`NGX_HTTP_SRV_CONF`表示可以出现在 server 块下，nginx 允许配置项出现在多个地方，比如`NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF`，这样的话，该配置项所属模块就必须决定由哪个地方的值为准，所以才需要 merge 操作。
而`conf`字段，用于指示配置项所处内存的相对偏移位置。这样说太抽象了，具体的说，每个 HTTP 模块都可以定义三种配置项结构体，分别用于存储 main、srv、loc 级别的配置项，所谓 main 级别的配置项，就是`create_main_conf`回调函数创建的配置项结构体。比如`ngx_http_core_module`就定义了`ngx_http_core_main_conf_t`, `ngx_http_core_srv_conf_t`和`ngx_http_core_loc_conf_t`，这三个结构体都是不一样的，它们分别由`create_main_conf`, `create_srv_conf`, `create_loc_conf`回调函数创建。nginx 框架自动解析时需要知道该把解析到的配置项的值写入到哪个结构体中，这就有`conf`成员来指示。 而对于`offset`字段，则是表明该配置项在所属结构体中的偏移量。可以使用`offsetof`宏来计算。

所谓 main 级别，也就是说，该配置项**最低**只能在 http{} 块中出现；而 srv 基本，则**最低**只能在 server{} 块中出现(所以也可以在 http{} 块中出现)；而 loc 级别，则**最低**只能在 location{} 块中出现(也就是说可以在 http{}, server{}, location{} 中同时出现，只要`ngx_command_t`结构体中的`type`字段设置了的话)。


上面说的**最低**这个要求，是一个逻辑上的要求，也就是我们把它划分为这三种要求中的某个要求。既然是逻辑上的要求，那么就得需要物理层面的设施来保障。怎么保障呢？type 不是可以随意设置的吗？

其实`type`字段不是随意设置的。在开发一个 http 模块的时候，我们需要把我们将要创建的配置项分门别类：

* 只能出现在 http{} 块中的配置项放到`ngx_http_xxx_main_conf_t`中去，由`create_main_conf`创建
* 只能出现在 server{} 和 http{} 块中的配置项放到`ngx_http_xxx_srv_conf_t`中去，由`create_srv_conf`创建。
* 可以出现在 location{}, server{} 和 http{} 块中的配置项放到`ngx_http_xxx_loc_conf_t`中去，由`create_loc_conf`创建。

这就完成了逻辑上的分类了，然后对不同的结构体使用不同的`type`字段来保障其**最低**的要求：

* 对`ngx_http_xxx_main_conf_t`结构体中的配置项的`type`字段设置为`NGX_HTTP_MAIN_CONF`
* 对`ngx_http_xxx_srv_conf_t`结构体中的配置项的`type`字段设置为`NGX_HTTP_SRV_CONF`，在有需要的情况下还可以并上`NGX_HTTP_MAIN_CONF`
* 对`ngx_http_xxx_loc_conf_t`结构体中的配置项的`type`字段设置为`NGX_HTTP_LOC_CONF`，在有需要的情况下还可以并上`NGX_HTTP_SRV_CONF`或者`NGX_HTTP_MAIN_CONF`

这也可以解释为什么在发现 server{} 块时不仅仅要调用`create_srv_conf`，还要调用`create_loc_conf`，因为 loc 级别的配置项也可以出现在 server{} 块下(当然也可以出现在 http{} 块下)，但是不会调用`create_main_conf`，因为 main 级别的配置项(`ngx_http_xxx_main_conf_t`结构体中的配置项)只能出现在 http{} 块下(因为它们的`type`字段设置的是`NGX_HTTP_MAIN_CONF`)。

上面说的`type`和配置项级别的关系在`ngx_http_core_module`模块中可以很清晰地看出，这个模块具有这三种级别的配置项，它们被分类放在不同的结构体中，而观察它`type`字段则可以观察出这个特点。

 **TODO: 为什么让配置项可以同时在不同块中出现，比如 location{} 和 server{}，这种 feature 是什么需求导致的？**

## 参考

[nginx main, srv, conf 三种配置级别-agentzh](https://groups.google.com/forum/#!topic/openresty/hSBkNvrHNXI)
