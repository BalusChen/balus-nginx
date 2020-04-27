# nginx HTTP 配置项管理（三）

由于 location 支持嵌套，所以这部分代码会比较复杂。

## location 的配置语法

在官网上可以找到`location`指令的配置语法：

```nginx
location [ = | ~ | ~* | ^~ ] uri { ... }
location @name { ... }
```

对于`location [ = | ~ | ~* | ^~ ] uri { ... }`，其中的`=/~/~*/^~`等符号被称为
modifier， 后面的`uri`被称为 name（或者称为路径、path），这里展示的是 modifier
和 name 之间是有空格的，

```nginx
location ~ \.flv {
    flv;
}
```

这是最正规的写法（说正规是因为 nginx 最开始支持的就是这种）；后续 nginx 也支持
modifier 和 name 之间不带空格而连在一起：

```nginx
location ~\.flv {
    flv;
}
```

而对于`location @name {...}`这种语法，这种 location 被称为命名 location，它不
用于通常的 HTTP 请求处理，而是在 nginx 内部重定向使用，比如`try_files`和
`error_pages`等：

> The “@” prefix defines a named location. Such a location is not used for a
> regular request processing, but instead used for request redirection. They
> cannot be nested, and cannot contain nested locations.

这种命名 location 的 name 和`@`必须连在一起。

### location 的种类

每种 modifier 代表一种 name，从而代表着一类 location：

* `=`：精确匹配，比如`location = / { ... }`
* `~`：区分大小写的正则表达式，比如`location ~ \.flv { ... }`
* `~*`：不区分大小写的正则表达式，比如`location ~* \.(png|jpg|jpeg) { ... }`
* `^~`：抢占式前缀匹配，比如`location ^~ /laputa { ... }`
* 不带 modifier：普通前缀匹配，比如`location ^~ /star/detail { ... }`
* 没有 name：这种在`location`中是不合法的，但是像`if`、`limit_except`等指令也是
被编译为`ngx_http_core_loc_conf_t`，但是不带 name/path
* 命名路径：像`location @balus { ... }`，这种也是不带 modifier 的

那么 nginx 怎么来标识/区分这几种 location 呢，看代码：

```c
static char *
ngx_http_core_location(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    /* create loc conf */
    ...

    value = cf->args->elts;

    if (cf->args->nelts == 3) {

        len = value[1].len;
        mod = value[1].data;
        name = &value[2];

        if (len == 1 && mod[0] == '=') {

            clcf->name = *name;
            clcf->exact_match = 1;

        } else if (len == 2 && mod[0] == '^' && mod[1] == '~') {

            clcf->name = *name;
            clcf->noregex = 1;

        } else if (len == 1 && mod[0] == '~') {

            if (ngx_http_core_regex_location(cf, clcf, name, 0) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

        } else if (len == 2 && mod[0] == '~' && mod[1] == '*') {

            if (ngx_http_core_regex_location(cf, clcf, name, 1) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid location modifier \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

    } else {

        name = &value[1];

        if (name->data[0] == '=') {

            clcf->name.len = name->len - 1;
            clcf->name.data = name->data + 1;
            clcf->exact_match = 1;

        } else if (name->data[0] == '^' && name->data[1] == '~') {

            clcf->name.len = name->len - 2;
            clcf->name.data = name->data + 2;
            clcf->noregex = 1;

        } else if (name->data[0] == '~') {

            name->len--;
            name->data++;

            if (name->data[0] == '*') {

                name->len--;
                name->data++;

                if (ngx_http_core_regex_location(cf, clcf, name, 1) != NGX_OK) {
                    return NGX_CONF_ERROR;
                }

            } else {
                if (ngx_http_core_regex_location(cf, clcf, name, 0) != NGX_OK) {
                    return NGX_CONF_ERROR;
                }
            }

        } else {

            clcf->name = *name;

            if (name->data[0] == '@') {

                clcf->named = 1;
            }
        }
    }

    ...
}
```

其中考虑了 modifier 和 name 有无空格的两种情况，并且对于每种 location，
`ngx_http_core_loc_conf_t`中都有相应的字段来标识：

```c
struct ngx_http_core_loc_conf_s {
    ngx_str_t     name;          /* location name */

#if (NGX_PCRE)
    ngx_http_regex_t  *regex;
#endif

    unsigned      noname:1;   /* "if () {}" block or limit_except */
    unsigned      lmt_excpt:1;
    unsigned      named:1;

    unsigned      exact_match:1;
    unsigned      noregex:1;

    ...
};
```

location 的标识关系如下：

* 常规的前缀匹配：无
* 独占式前缀匹配：`noregex = 1`
* 正则表达式：`regex != NULL`
* 精确匹配：`exact_match = 1`
* 命名 location：`named = 1`
* 无名 location：`noname = 1`

### location 嵌套的一些限制

`location`是可以嵌套的，但是不能以任意方式/组合嵌套，还是有一些限制的，这个也是在解析
`location`指令时做的：

```c
static char *
ngx_http_core_location(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    /* 紧接着上一段代码 */

    pclcf = pctx->loc_conf[ngx_http_core_module.ctx_index];

    if (cf->cmd_type == NGX_HTTP_LOC_CONF) {

        /* nested location */

        if (pclcf->exact_match) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "location \"%V\" cannot be inside "
                               "the exact location \"%V\"",
                               &clcf->name, &pclcf->name);
            return NGX_CONF_ERROR;
        }

        if (pclcf->named) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "location \"%V\" cannot be inside "
                               "the named location \"%V\"",
                               &clcf->name, &pclcf->name);
            return NGX_CONF_ERROR;
        }

        if (clcf->named) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "named location \"%V\" can be "
                               "on the server level only",
                               &clcf->name);
            return NGX_CONF_ERROR;
        }

        len = pclcf->name.len;

#if (NGX_PCRE)
        if (clcf->regex == NULL
            && ngx_filename_cmp(clcf->name.data, pclcf->name.data, len) != 0)
#else
        if (ngx_filename_cmp(clcf->name.data, pclcf->name.data, len) != 0)
#endif
        {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "location \"%V\" is outside location \"%V\"",
                               &clcf->name, &pclcf->name);
            return NGX_CONF_ERROR;
        }
    }

    ...
}
```

很容易理解，而且根据其出错的 log 信息可以总结以下几个限制：

* 精确匹配的`location{...}`不允许嵌套其他`location{...}`
* 命名 location 下不允许嵌套其他`location{...}`
* 命名 location 只能出现在`server{...}`下，而不能嵌套在其他`location{...}`中
* 如果内部 location 没有使用正则表达式的话，内部的 location 的名字必须以外部 location
的名字为前缀

## location tree 的构建

前面只是一些基础知识，location tree 的构建才是大头，依旧紧接着上面【location 嵌套限制】
部分的代码：

```c
static char *
ngx_http_core_location(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    if (ngx_http_add_location(cf, &pclcf->locations, clcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    save = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_HTTP_LOC_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    return rv;
}
```

这里将`server/location`下的所有`location{...}`的`ngx_http_core_loc_conf_t`结构串成
一个`ngx_queue_t`双向链表，然后在 解析完了`http{...}`内部的所有指令之后开始构建 location
tree：

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

解析`http`指令这部分代码太多，就只列了一下主要流程，其中在`merge_servers`之后开始构建
location tree，这个部分分为两个步骤：

### 排序 location

由于 location 是可以嵌套的，所以这部分代码免不了需要递归，还是有些复杂。

```c
static ngx_int_t
ngx_http_init_locations(ngx_conf_t *cf, ngx_http_core_srv_conf_t *cscf,
    ngx_http_core_loc_conf_t *pclcf)
{
    ...

    locations = pclcf->locations;

    if (locations == NULL) {
        return NGX_OK;
    }

    ngx_queue_sort(locations, ngx_http_cmp_locations);
```

首先`if`判断是递归的出口，然后`ngx_queue_sort`对直属于本`location{...}/server{...}`
下的所有的`location{...}`进行排序，传入的比较器为`ngx_http_cmp_locations`：

```c
static ngx_int_t
ngx_http_cmp_locations(const ngx_queue_t *one, const ngx_queue_t *two)
{
    ngx_int_t                   rc;
    ngx_http_core_loc_conf_t   *first, *second;
    ngx_http_location_queue_t  *lq1, *lq2;

    lq1 = (ngx_http_location_queue_t *) one;
    lq2 = (ngx_http_location_queue_t *) two;

    first = lq1->exact ? lq1->exact : lq1->inclusive;
    second = lq2->exact ? lq2->exact : lq2->inclusive;

    if (first->noname && !second->noname) {
        /* shift no named locations to the end */
        return 1;
    }

    if (!first->noname && second->noname) {
        /* shift no named locations to the end */
        return -1;
    }

    if (first->noname || second->noname) {
        /* do not sort no named locations */
        return 0;
    }

    if (first->named && !second->named) {
        /* shift named locations to the end */
        return 1;
    }

    if (!first->named && second->named) {
        /* shift named locations to the end */
        return -1;
    }

    if (first->named && second->named) {
        return ngx_strcmp(first->name.data, second->name.data);
    }

#if (NGX_PCRE)

    if (first->regex && !second->regex) {
        /* shift the regex matches to the end */
        return 1;
    }

    if (!first->regex && second->regex) {
        /* shift the regex matches to the end */
        return -1;
    }

    if (first->regex || second->regex) {
        /* do not sort the regex matches */
        return 0;
    }

#endif

    rc = ngx_filename_cmp(first->name.data, second->name.data,
                          ngx_min(first->name.len, second->name.len) + 1);

    if (rc == 0 && !first->exact_match && second->exact_match) {

        /* an exact match must be before the same inclusive one */
        return 1;
    }

    return rc;
}
```

对于`ngx_queue_sort(q, cmp)`，如果`cmp(a, b) < 0`，那么`a`在`b`的前面。比较器会
比较前面在解析`location`指令时根据这个 location 的类型而在
`ngx_http_core_loc_conf_t`中设置的几个字段：

1. 比较`noname`字段，`noname == 1`的在后，同为`noname == 1`则保持二者原有顺序；
同为`noname == 0`则继续比较下一个字段
2. 比较`named`字段，`named == 1`的在后，同为`named = 1`则保持二者原有顺序；同为
`named == 0`则继续比较下一个字段
3. 比较`regex`字段，`regex != NULL`的在后，同为`regex != NULL`则保持二者原有顺序；
同为`regex == NULL`则继续比较下一个字段
4. 比较 location name

比较器返回 0 的话，由于`ngx_queue_sort`是 stable 的，所以这种情况下会保持原有的
相对顺序。

在第四步比较 location name 时，比较的是`min(first->name.len, second->name.len) + 1`，
这里用了一个小技巧，location name 是以 0 字符结尾的，所以只要有一个更长，那么那么
这个location name 多出来的字符肯定比 0 字符大（所以`rc != 0`）。而只有完全相同的
location name 相比较才会 `rc == 0`

TODO：这里我还有一个问题，感觉少了一个`if (rc == 0 && first->exact_match && !second->exact_match)`
的判断。

排完序之后直属于`server{...}/location{...}`下的所有`location{...}`的顺序是这样的：

1. 精确匹配和两类前缀匹配（字典序）
2. 正则表达式 location（出现序）
3. 命名 location（出现序）
4. 无名 location（出现序）

```c
    named = NULL;
    n = 0;
#if (NGX_PCRE)
    regex = NULL;
    r = 0;
#endif

    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        clcf = lq->exact ? lq->exact : lq->inclusive;

        if (ngx_http_init_locations(cf, NULL, clcf) != NGX_OK) {
            return NGX_ERROR;
        }

#if (NGX_PCRE)

        if (clcf->regex) {
            r++;

            if (regex == NULL) {
                regex = q;
            }

            continue;
        }

#endif

        if (clcf->named) {
            n++;

            if (named == NULL) {
                named = q;
            }

            continue;
        }

        if (clcf->noname) {
            break;
        }
    }
```

然后在一个`for`循环中对每个节点，都递归调用自身，把这个节点代表的`location{...}`
内嵌的`location{...}`也给递归初始化，这个没什么好说的。

`for`循环主要做的是记录下带正则的 location、命名 location 和无名 location 的起始
位置。遇到无名 location 就`break`出循环，因为根据前面的排序，无名 location 排在最
后面。

记录下这几个位置是用来做什么呢？接着看代码：

```c
    if (q != ngx_queue_sentinel(locations)) {
        ngx_queue_split(locations, q, &tail);
    }

    if (named) {
        clcfp = ngx_palloc(cf->pool,
                           (n + 1) * sizeof(ngx_http_core_loc_conf_t *));
        if (clcfp == NULL) {
            return NGX_ERROR;
        }

        cscf->named_locations = clcfp;

        for (q = named;
             q != ngx_queue_sentinel(locations);
             q = ngx_queue_next(q))
        {
            lq = (ngx_http_location_queue_t *) q;

            *(clcfp++) = lq->exact;
        }

        *clcfp = NULL;

        ngx_queue_split(locations, named, &tail);
    }

#if (NGX_PCRE)

    if (regex) {

        clcfp = ngx_palloc(cf->pool,
                           (r + 1) * sizeof(ngx_http_core_loc_conf_t *));
        if (clcfp == NULL) {
            return NGX_ERROR;
        }

        pclcf->regex_locations = clcfp;

        for (q = regex;
             q != ngx_queue_sentinel(locations);
             q = ngx_queue_next(q))
        {
            lq = (ngx_http_location_queue_t *) q;

            *(clcfp++) = lq->exact;
        }

        *clcfp = NULL;

        ngx_queue_split(locations, regex, &tail);
    }

#endif

    return NGX_OK;
}
```

首先检查这个链表中是否有无名 location，有的话，就直接将其丢弃。这里是用
`ngx_queue_split(locations, q, tail)`，因为只有遇到了无名 location，才会`break`
出循环，才会导致`q != sentinel`，而根据排序规则，无名 location 在链表的最后一部
分。所以用 split 将其分割出来，得到的`tail`为无名 location 循环链表的 sentinle，
而`q`为链表头。但是其实`tail`没有用到，所以其实是丢弃了所有的无名 location。

为什么要丢弃呢？因为无名的 location 实际上是不存在的，而只是`if`、`limit_except`
等指令编译成了`ngx_http_core_loc_conf`使用，并不是真正的`location`指令。

然后检查是否有命名 location，因为前面已经把无名 location 给分割出来了，在加上排序
的结果， 就可以保证此时`[named, sentinel)`都是命名 location，所有的命名 location
都被放在`cscf->named_location`中，这是因为命名 location 必须直属于`server{...}`块，
而不被嵌入到其他`location{...}`中

最后检查是否有带正则的 location，有的话，将其放入上一级的`clcf->regex_locations`
数组中去。这个的原理和前面类似。

所以其实这 3 个 if 的顺序是不能变的，只有前面的`if`把链表尾部的那一类 location
给分割出来之后，才能进行下一次分割。这三次分割的`tail`字段都没有用上，因为这只是
`ngx_queue_split`函数需要

### 构建 location tree

经过了`ngx_http_init_locations`的排序+归类之后，`clcf->locations`链表中只剩下精确
匹配、前缀匹配、抢占式前缀匹配这 3 类 location 了。

```c
static ngx_int_t
ngx_http_init_static_location_trees(ngx_conf_t *cf,
    ngx_http_core_loc_conf_t *pclcf)
{
    ngx_queue_t                *q, *locations;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_location_queue_t  *lq;

    locations = pclcf->locations;

    if (locations == NULL) {
        return NGX_OK;
    }

    if (ngx_queue_empty(locations)) {
        return NGX_OK;
    }

    for (q = ngx_queue_head(locations);
         q != ngx_queue_sentinel(locations);
         q = ngx_queue_next(q))
    {
        lq = (ngx_http_location_queue_t *) q;

        clcf = lq->exact ? lq->exact : lq->inclusive;

        if (ngx_http_init_static_location_trees(cf, clcf) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    if (ngx_http_join_exact_locations(cf, locations) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_http_create_locations_list(locations, ngx_queue_head(locations));

    pclcf->static_locations = ngx_http_create_locations_tree(cf, locations, 0);
    if (pclcf->static_locations == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}
```

和前面一样，在`for`循环中深度优先递归处理内嵌的`location{...}`，没什么说的。后面
一次调用了 3 个函数，分别来讲一下：

#### 合并同名 location

```c
static ngx_int_t
ngx_http_join_exact_locations(ngx_conf_t *cf, ngx_queue_t *locations)
{
    ngx_queue_t                *q, *x;
    ngx_http_location_queue_t  *lq, *lx;

    q = ngx_queue_head(locations);

    while (q != ngx_queue_last(locations)) {

        x = ngx_queue_next(q);

        lq = (ngx_http_location_queue_t *) q;
        lx = (ngx_http_location_queue_t *) x;

        if (lq->name->len == lx->name->len
            && ngx_filename_cmp(lq->name->data, lx->name->data, lx->name->len)
               == 0)
        {
            if ((lq->exact && lx->exact) || (lq->inclusive && lx->inclusive)) {
                ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                              "duplicate location \"%V\" in %s:%ui",
                              lx->name, lx->file_name, lx->line);

                return NGX_ERROR;
            }

            lq->inclusive = lx->inclusive;

            ngx_queue_remove(x);

            continue;
        }

        q = ngx_queue_next(q);
    }

    return NGX_OK;
}
```

从表头遍历到表尾，检查是否有同名的 location，由于前面已经排好序了，所以同名的话只
会出现在相邻位置，故而只需要检查顺序检查相邻两个节点即可。

如果碰到相邻两个 location name 相同，那么得进行合法性检验。不能两个都是`exact`或
者`inclusive`。

如果这两个同名 location 是合法的，也就是一个是挂在`exact`上，一个是挂在`inclusive`
上，那么就把`inclusive`的挂载在`exact`节点的的`inclusive`指针上，并将其从链表中移
除。

(TODO: 这里需要 polish 一下，exact 的 xxx 和 挂在 exact 上感觉总是有歧义)

##### 合法性校验

那么什么样的 location 是`exact`的，什么样的是`inclusive`的呢？这个是在解析`location`
时调用`ngx_http_add_location`做的：

```c
ngx_http_add_location(ngx_conf_t *cf, ngx_queue_t **locations,
    ngx_http_core_loc_conf_t *clcf)
{
    ...

    if (clcf->exact_match
#if (NGX_PCRE)
        || clcf->regex
#endif
        || clcf->named || clcf->noname)
    {
        lq->exact = clcf;
        lq->inclusive = NULL;

    } else {
        lq->exact = NULL;
        lq->inclusive = clcf;
    }

    ...
}
```


可以看到，只有前缀匹配为`inclusive`，其他的都是`exact`。

来看看为什么同名的 location 不能同为`exact`或者同为`inclusive`：
 
```nginx
server {
    listen              9877;
    server_name         laputa;

    location /hello {
        ...
    }

    location /hello {

    }

    location ^~ /hello {
    }

}
```

这 3 个都是`inclusive`的， 一个请求`curl http://laputa:9877/hello/world`过来，
选哪个呢？没法确定，所以这是不行的。

```nginx
server {
    listen              9877;
    server_name         laputa;

    location = /hello {
        ...
    }

    location = /hello {
    }
}
```

这两个都是`exact`的，一个请求`curl http://laputa:9877/hello`过来，也不能确定选择
该选择哪个，这也是不合法的。

只有像下面这样，一个`exact`，一个`inclusive`的才行：

```nginx
server {
    listen              9877;
    server_name         laputa;

    location = /hello {
        ...
    }

    location /hello {
        ...
    }
}
```

一个请求`curl http://laputa:9877/hello`，这个是 ok 的。

#### 构造前缀树/list

`ngx_http_create_locations_list`和`ngx_http_create_locations_tree`这两个函数是用
来构造二叉查找树的。

前面我们看到了`ngx_http_location_queue_t`结构：

```c
typedef struct {
    ngx_queue_t                      queue;
    ngx_http_core_loc_conf_t        *exact;
    ngx_http_core_loc_conf_t        *inclusive;
    ngx_str_t                       *name;
    u_char                          *file_name;
    ngx_uint_t                       line;
    ngx_queue_t                      list;
} ngx_http_location_queue_t;
```

其中有一个`list`字段还没有接触到，`ngx_http_create_locations_list`中的`list`就是
指的这个字段。这个函数的作用是将以 A location 的 name 为前缀的所有 location 都链
接到它的`list`字段中：

这个函数也是一个递归的，而且递归调用的地方比较多，所以理解起来可能有一定的困难。
但是首先需要注意的是，这里的递归和调用它的`ngx_http_init_static_locations_tree`
中的递归性质是不一样。像`ngx_http_init_static_locations_tree`以及以及前面说的其他
函数的递归都是为了处理嵌套的`location`，但是这里不是，这里只处理同一级的`location`，
嵌套的情况由`ngx_http_init_static_locations_tree`自己递归来处理。

那么这里为什么要递归，而且多个地方递归呢？我们首先来直观感受一下：

![nginx-http-locaion-list](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/directives/nginx-http-location-list.png)

这里没有很精确地画出`ngx_queue_t`双向链表结构出来，但是并不影响。这里处理的是前缀
匹配的 location，所以精确匹配的不考虑在内（而实际上经过前面几轮操作后`locations`
链表中只剩精确匹配和前缀匹配了），而且前面排序过，精确匹配在前缀匹配之前，他们内
部都是按照字典序排序。

```c
static void
ngx_http_create_locations_list(ngx_queue_t *locations, ngx_queue_t *q)
{
    ...
    lq = (ngx_http_location_queue_t *) q;

    if (lq->inclusive == NULL) {
        ngx_http_create_locations_list(locations, ngx_queue_next(q));
        return;
    }
    ...
```

所以这种情况就是跳过精确匹配的情况。

后面就全部都是 inclusive 的了，由于是字典序排序，所以前缀相同的节点肯定都排在一
起。所以对于一个节点，往后找以该节点名为前缀的所有节点，将其从`locations`链表移动
到该节点的`list`链表中去。

但是对于`list`链表中的节点，可能还会存在着以某个节点名为前缀的情况，比如上图中的
`/c`的 list 中`/cab`、`/cac`这两个节点名就以`/ca`为前缀，所以需要对`list`递归处理。

```c
    ...
    len = lq->name->len;
    name = lq->name->data;

    for (x = ngx_queue_next(q);
         x != ngx_queue_sentinel(locations);
         x = ngx_queue_next(x))
    {
        lx = (ngx_http_location_queue_t *) x;

        /*
         * NOTE: 经过前面的一系列操作，现在 locations 链表中的节点和顺序：
         *       exact > inclusive
         *       而对于 exact 和 inclusive 同名的情况，则将 inclusive 移入 exact
         *       节点的 inclusive 指针中
         *       所以碰到第一个 name 不相等的就退出。
         */
        if (len > lx->name->len
            || ngx_filename_cmp(name, lx->name->data, len) != 0)
        {
            break;
        }
    }

    q = ngx_queue_next(q);

    /*
     * NOTE: 如果 q == x，那么说明一次循环都没有进入，也就是说没有其他 location
     *       以 q 的名字为前缀，那么从 x（也就是 q->next）开始继续递归处理以 x 的
     *       名字为前缀的节点
     */
    if (q == x) {
        ngx_http_create_locations_list(locations, x);
        return;
    }

    /*
     * NOTE：分割成 [locations, q) 和 [q, locations) 两部分
     *       其中 tail 是 [q, locations) 部分的 sentinel（tail->next = q）
     *
     * NOTE: 按照常理是应该把 [q, x) 给分割出来加入到 lq->list 的，这里还没有判断
     *       是否 x == locations，但是就算 x != locations 也没有什么问题，后面判
     *       断了再把 [x, locations) 从 lq->list 移除出来放到 locations 中去
     */
    ngx_queue_split(locations, q, &tail);
    ngx_queue_add(&lq->list, &tail);

    /*
     * NOTE: 如果 x == sentinel，说明从 [q, sentinel) 都以 q 的名字为前缀，那么直
     *       接递归处理 list 中更长的前缀
     */
    if (x == ngx_queue_sentinel(locations)) {
        ngx_http_create_locations_list(&lq->list, ngx_queue_head(&lq->list));
        return;
    }

    /*
     * NOTE: 这种是普通情况，就是一部分 [q, x) 以 q 的名字为前缀，另一部分
     *       [x, sentinel) 则不是，那么把 [q, x) 加入到 lq->list，并递归处理之，
     *       但是前面已经把 [q, locations) 都加入到了 lq->list，所以这里需要把
     *       [x, locations) 从 lq->list 中移除，放到 locations 链表中去
     */
    ngx_queue_split(&lq->list, x, &tail);
    ngx_queue_add(locations, &tail);

    ngx_http_create_locations_list(&lq->list, ngx_queue_head(&lq->list));

    ngx_http_create_locations_list(locations, x);
}
```

经过这次处理之后，对于所有前缀匹配的 location，以 location A 的名字为前缀的
location 都被挂载在 A 的`list`链表下。大致形成了一棵前缀树的形状。

#### 构造 location tree

前面只是大致有了一颗前缀树的形状，但是其实还是组织成了`list`链表，而且只是处理了
前缀匹配的 location，接下来要把所有的 location 构建出一颗静态三叉树。

树节点结构为`ngx_http_location_tree_node_t`：

```c
typedef struct ngx_http_location_tree_node_s  ngx_http_location_tree_node_t;

struct ngx_http_location_tree_node_s {
    ngx_http_location_tree_node_t   *left;
    ngx_http_location_tree_node_t   *right;
    ngx_http_location_tree_node_t   *tree;

    ngx_http_core_loc_conf_t        *exact;
    ngx_http_core_loc_conf_t        *inclusive;

    u_char                           auto_redirect;
    u_char                           len;
    u_char                           name[1];
};
```

其中除了`left`和`right`节点，还有一个`tree`节点。这个就是三叉树的由来。

为什么不直接用红黑树呢？因为对于 location 而言，我们不需要插入、删除等操作，所以
可以实现得更高效一些。

```c
static ngx_http_location_tree_node_t *
ngx_http_create_locations_tree(ngx_conf_t *cf, ngx_queue_t *locations,
    size_t prefix)
{
    size_t                          len;
    ngx_queue_t                    *q, tail;
    ngx_http_location_queue_t      *lq;
    ngx_http_location_tree_node_t  *node;

    q = ngx_queue_middle(locations);

    lq = (ngx_http_location_queue_t *) q;
    len = lq->name->len - prefix;

    node = ngx_palloc(cf->pool,
                      offsetof(ngx_http_location_tree_node_t, name) + len);
    if (node == NULL) {
        return NULL;
    }

    node->left = NULL;
    node->right = NULL;
    node->tree = NULL;
    node->exact = lq->exact;
    node->inclusive = lq->inclusive;

    node->auto_redirect = (u_char) ((lq->exact && lq->exact->auto_redirect)
                           || (lq->inclusive && lq->inclusive->auto_redirect));

    node->len = (u_char) len;
    ngx_memcpy(node->name, &lq->name->data[prefix], len);

```

将一棵有序链表转换成一棵二叉树，直观的做法就取链表中间节点作为 root，
然后递归地以链表的左、右部分来构建左子树和右子树。

这里的做法也是类似，首先`ngx_queue_middle`拿到`locations`链表的中间节点。
然后初始化其中的一些字段，这里的`len = lq->name->len - prefix`和
`ngx_memcpy(node->name, &lq->name->data[prefix], len)` 这两块可能有点
疑惑，现在先不管，等解析完了大体流程再解释这一部分。


```c
    ngx_queue_split(locations, q, &tail);

    if (ngx_queue_empty(locations)) {
        goto inclusive;
    }

    node->left = ngx_http_create_locations_tree(cf, locations, prefix);
    if (node->left == NULL) {
        return NULL;
    }

    ngx_queue_remove(q);

    if (ngx_queue_empty(&tail)) {
        goto inclusive;
    }

    node->right = ngx_http_create_locations_tree(cf, &tail, prefix);
    if (node->right == NULL) {
        return NULL;
    }
```

然后就是按照前面说的，再链表中间切分，递归地构建左右子树。这里需要注意切分完了之
后链表为空的情况。切分完了之后`[location, q)`部分由`location`担任哨兵节点，
`[q, locations)`部分的哨兵节点则由`tail`担任。

`ngx_queue_middle`对于节点个数为奇数的情况是会返回中间节点，对于偶数的情况，则会
返回第二部分的第一个节点。所以对于奇数的情况`ngx_queue_split(locations, q,&tail)`
得到的`[q, locations)`部分要比`[locations, q)`部分多一个节点。如果左边部分为空的
话，那么右边部分只有一个元素，就不用从右边部分`ngx_queue_remove(q)`移除`q`作为
root 了，毕竟只有一个节点。

FIXME: 感觉上一顿解释的正确，`ngx_queue_remove(q)`操作会把`q->next`和`q->prev`都
置为`NULL`，没有这个操作的话，感觉后面有问题，应该是这里还有一些地方没有理解。而
且感觉代码里面的这段注释也有问题（比如`locations`链表只有一个有效节的情况）：


> ngx_queue_split() insures that if left part is empty,
> then right one is empty too


```c
inclusive:

    if (ngx_queue_empty(&lq->list)) {
        return node;
    }

    node->tree = ngx_http_create_locations_tree(cf, &lq->list, prefix + len);
    if (node->tree == NULL) {
        return NULL;
    }

    return node;
}
```

前面说了构建了`left`和`right`这二叉，这第三叉就是`tree`节点，这个节点存储的是前缀
匹配的 location。这里其他地方和`left`和`right`的处理是一样的，但是有一个`prefix`
的概念，这个就是`tree`这个前缀树所特有的：

```c
    lq = (ngx_http_location_queue_t *) q;
    len = lq->name->len - prefix;
    ...
    node->len = (u_char) len;
    ngx_memcpy(node->name, &lq->name->data[prefix], len);
    ...
    node->tree = ngx_http_create_locations_tree(cf, &lq->list, prefix + len);
    if (node->tree == NULL) {
    return NULL;
    }
```

这几句要结合起来一起看。我们知道 location `lq`的`list`链表都是以`lq`的名字为前缀
的，所以在 node 中存储 location 的名字时就不必把上一个节点的名字重复存储进去了。
而`prefix`参数就是指明前缀名的长度的。

在`ngx_http_init_static_location_trees`函数中初始调用本函数时传入的`prefix`为 0。
在处理`left`和`right`节点时，都把这个参数原封不动地传下去；而处理`tree`前缀树时，
则传递的`prefix + len = prefix + (lq->name->len + prefix) = lq->name->prefx`，也
就是本节点的 name 的长度，后面`lq->list`中的节点在设置 node 的`name`字段时，只会
把从`lq->name->data[prefix]`开始拷贝，这就避免了把父节点的 name 重复存储了，最后
来看一张图就明白了：

![nginx-http-static-locations-tree](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/directives/nginx-http-static-locations-tree.png)

#### 在 static locations tree 中查找对应的 location

查找 location 也是一大块代码，毕竟 location 除了这里的 static 部分，还有带正则的，
 但是这里主要关注 static locations tree 的查找流程，来理解前面为什么要这个`prefix`

```c
/*
 * NGX_OK       - exact match
 * NGX_DONE     - auto redirect
 * NGX_AGAIN    - inclusive match
 * NGX_DECLINED - no match
 */

static ngx_int_t
ngx_http_core_find_static_location(ngx_http_request_t *r,
    ngx_http_location_tree_node_t *node)
{
    u_char     *uri;
    size_t      len, n;
    ngx_int_t   rc, rv;

    len = r->uri.len;
    uri = r->uri.data;
```

这里可以看到，location 的查找是通过 uri 来确定的。

```c
    rv = NGX_DECLINED;

    for ( ;; ) {

        if (node == NULL) {
            return rv;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "test location: \"%*s\"",
                       (size_t) node->len, node->name);

        n = (len <= (size_t) node->len) ? len : node->len;

        rc = ngx_filename_cmp(uri, node->name, n);

        if (rc != 0) {
            node = (rc < 0) ? node->left : node->right;

            continue;
        }
```

首先和节点的`node`字段相比较，

```c
        if (len > (size_t) node->len) {

            if (node->inclusive) {

                r->loc_conf = node->inclusive->loc_conf;
                rv = NGX_AGAIN;

                node = node->tree;
                uri += n;
                len -= n;

                continue;
            }

            /* exact only */

            node = node->right;

            continue;
        }

        if (len == (size_t) node->len) {

            if (node->exact) {
                r->loc_conf = node->exact->loc_conf;
                return NGX_OK;

            } else {
                r->loc_conf = node->inclusive->loc_conf;
                return NGX_AGAIN;
            }
        }

        /* len < node->len */

        if (len + 1 == (size_t) node->len && node->auto_redirect) {

            r->loc_conf = (node->exact) ? node->exact->loc_conf:
                                          node->inclusive->loc_conf;
            rv = NGX_DONE;
        }

        node = node->left;
    }
}
```

## 参考

[nginx 官方文档：location 指令](http://nginx.org/en/docs/http/ngx_http_core_module.html#location)

[nginx 从入门到精通](https://tengine.taobao.org/book/chapter_11.html#location)

[nginx 源代码笔记-URI 匹配](https://ialloc.org/blog/ngx-notes-http-location/)

[Nginx源码阅读笔记-查询HTTP配置流程](https://www.codedump.info/post/20190212-nginx-http-config/)

[可视化 nginx location 的匹配过程](https://github.com/detailyang/nginx-location-match-visible)
