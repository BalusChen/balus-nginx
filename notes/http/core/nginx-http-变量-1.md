# Nginx 变量（一）内部变量

本来前段时间一直在看 upstream 和负载均衡，但是中间一段时间又好久没看，有点不知道怎么看下去了。所以打算看点新的知识点，正好感觉《深入》这本书里面变量章节不长，而且这次年假时间比较长，我应该可以好好地消化它。(当然后面会继续把负载均衡部分给消化掉的，说到做到！)

## 内部变量与外部变量

首先来看一段 nginx.conf 配置：

```nginx
log_format main '$remote_addr $remote_user'
		' [$time_local] "$request" $status'
		' $host $body_bytes_sent $gzip_ration "$http_referer"'
		' "$http_user_agent" "$http_x_forwarded_for"';
access_log logs/access.log main;
```

上面是在通过`log_format`这个配置项来定义日志格式，其中用到了`$remote_addr`这种字符串，这就是变量。

变量是用来表达实时请求中某些共性参数的一种方式，比如说，对于一个请求(即一个连接)，总有“对端 ip 的概念”，在限速时，我们就可以依据对端 IP 来进行限速：

```nginx
limit_req_zone $binary_remote_addr zone=one:10m rate=1r/s;
```

“对端 IP”是一个共性概念，每个请求都有，但是每个请求都可能不一样。

上面都是**内部变量**的例子，既然有内部变量，那肯定还有外部变量，那么这两者的区别是什么呢：

* 内部变量：在 C 代码中定义的变量
* 外部变量：变量名称是在 nginx.conf 配置文件中声明的，而不是在 C 源码中定义的

```c
set $param1 "abcd";
set $memcached_key "$uri$args"
```

## 内部变量的工作原理

理解内部变量的设计要从其应用场景入手。内部变量是在 Nginx 的代码内部定义的，也就是说它是在 Nginx 模块在 C 代码中定义的。在 C 语言中，一个变量通常有“声明”、“定义”、和“使用”三个阶段。这里说的变量定义其实应该是“声明”阶段，即用变量名来声明这个变量的存在，而没有实际为这个变量值分配内存。那什么时候为变量值分配内存呢？nginx 采用的方法是只有用到了这个变量的时候才会为这个变量值分配内存。这样做是有原因的：

* 对于不同的请求，变量值是不同的。但是对于一个请求，一般只会用到非常小的一部分变量，如果全部都提前分配内存，会得不偿失。

在 nginx 中，变量被分为**变量名**和**变量值**两部分，二者分别用不同的数据结构来表示：

```c
typedef void (*ngx_http_set_variable_pt) (ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
typedef ngx_int_t (*ngx_http_get_variable_pt) (ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
typedef struct ngx_http_variable_s  ngx_http_variable_t;

struct ngx_http_variable_s {
    ngx_str_t                     name;   /* must be first to build the hash */
    ngx_http_set_variable_pt      set_handler;
    ngx_http_get_variable_pt      get_handler;
    uintptr_t                     data;
    ngx_uint_t                    flags;
    ngx_uint_t                    index;
};

```

其中变量名用`ngx_http_variable_t`结构体来表示，各字段释义：

* `name`: 变量名(不包括$)
* `set_handler`: 内部变量允许在 nginx.conf 文件中以`set`方式重新设置其值，那么可以实现该方法
* `get_handler`: 通过这个函数可以获取到变量的值
* `data`: 传递给 set_handler/get_handler 使用
* `flags`: 变量的特性
* `index`: 变量值在`ngx_http_request_t`结构中缓存数组的下标

其中 flags 目前(nginx 1.17.6)中有 6 个：

```c
#define NGX_HTTP_VAR_CHANGEABLE   1
#define NGX_HTTP_VAR_NOCACHEABLE  2
#define NGX_HTTP_VAR_INDEXED      4
#define NGX_HTTP_VAR_NOHASH       8
#define NGX_HTTP_VAR_WEAK         16
#define NGX_HTTP_VAR_PREFIX       32
```

这 6 个 flag 的含义如下：TODO

变量值通过`ngx_http_variable_value_t`结构表示：

```c

typedef ngx_variable_value_t  ngx_http_variable_value_t;
typedef struct {
    unsigned    len:28;

    unsigned    valid:1;
    unsigned    no_cacheable:1;
    unsigned    not_found:1;
    unsigned    escape:1;

    u_char     *data;
} ngx_variable_value_t;
```

其中各个字段的含义如下：

* `len`: 变量值必须是在一段连续内存中的字符串，字符串的长度就是 len
* `valid`: 为 1 表示当前这个变量已经被解析过了，而且数据是可用的
* `no_cacheable`: 为 1 表示不可缓存，而`NGX_HTTP_VAR_NOCACHEABLE`标志位相关
* `not_found`: 为 1 表示当前这个变量已经解析过，但是没有解析到相应的值
* `escape`: 仅由 ngx_http_log_module 使用，用于日志格式的字符转义
* `data`: 指向变量值所在内存的起始地址

### 内部变量的存储方式

前面都提到了**索引**、**缓存**这几个词，这就涉及到内部变量的存储方式了。变量名是所有请求都共有的，而变量值则随着请求的不同而不同；所以可以想象变量名应该是存在一个全局的变量当中的，而变量值则应该存储在`ngx_http_request_t`中的。

事实上和这个差不多，有一个需要注意的地方就是因为`ngx_http_variable_t`虽然说是变量名的数据结构体，但是通过其中的`get_handler`我们也可以得到变量值，所以其实不一定非得把变量值存储在请求结构体中，但是每次取值都用`get_handler`来解析就太慢了，所以我们可以在第一次解析的时候把变量值给存储在`ngx_http_request_t`结构中，然后后面每次取值都直接从请求结构体中取就可以了，这样就减少了解析时间。这就是**缓存**

那么**索引**是什么意思呢？变量名是字符串形式的，通过变量名取值的最简单的方法就是通过逐字符的比较，找到对应的`ngx_http_variable_t`结构，然后通过`get_handler`获取到变量值；更好一点的方法就是用哈希表来加速查找，但是由于可能存在冲突(链地址法)而不得不继续逐字符进行比较(但是比 naive 的方法会少很多比较)；最好的方法就是把变量名给映射到一个数组里面去，然后通过数组下标来取。

```c
typedef struct {
		...
    ngx_hash_t                 variables_hash;
    ngx_array_t                variables;         /* ngx_http_variable_t */
    ngx_array_t                prefix_variables;  /* ngx_http_variable_t */
    ngx_hash_keys_arrays_t    *variables_keys;
  	...

} ngx_http_core_main_conf_t;

struct ngx_http_request_s {
  	...
    ngx_http_variable_value_t        *variables;
  	...
}

```

来看看 nginx 实际是怎么做的。变量名的确是被放在一个全局的`ngx_http_core_main_conf_t`中的，而且的确是用了哈希表来加速的(`variables_hash`)，其中`variables_keys`是用于构造`variables_hash`的初始结构体，在其构建成功之后`variables_keys`就功成身退。

那么`variables`这个数组是用来干啥的呢(先不说`prefix_variables`)？这个就是和索引相关了。前面已经说过 nginx 默认会用哈希表给所有普通变量(说普通是因为还有几类特殊变量)加速，但是默认是不会使用索引的，索引的使用与否是由使用它的模块决定的，而不是由定义它的模块决定。当我们需要将一个变量名索引化的时候，就把他加入到`cmcf->variables`数组中去，然后在该数组中的下标就是其索引了，这样后面就可以直接通过索引来取变量名了，而不用哈希了。

而且其实只有索引化了的变量名的变量值才可以被缓存，只在哈希表而不在数组中的变量名的值是无法被缓存的。因为`ngx_http_request_t`结构体中的`variables`数组是一一对应的：

```c
static ngx_http_request_t *
ngx_http_alloc_request(ngx_connection_t *c)
{
  	...
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);

    r->variables = ngx_pcalloc(r->pool, cmcf->variables.nelts
                                        * sizeof(ngx_http_variable_value_t));
    if (r->variables == NULL) {
        ngx_destroy_pool(r->pool);
        return NULL;
    }
  	...
}
```

## 内部变量的初始化流程

内部变量的初始化流程和 HTTP 框架的初始化流程是息息相关的。下面从变量的角度来描绘一下 HTTP 框架的初始化流程：

![nginx-internal-variables-initialization](https://raw.githubusercontent.com/BalusChen/Markdown_Photos/master/Nginx/HTTP/variables/nginx-internal-variables-initialization.png)

那么开发 Nginx 模块时，在何时何地定义变量呢？因为变量的赋值等工作都是由 Nginx 框架来做的，所以 Nginx 的 HTTP 框架要求**所有的 HTTP 模块都必须在 preconfiguration 回调方法中定义新的变量**。这需要结合 HTTP 框架的初始化来理解。

* 首先是调用`create_(main/srv/loc)_conf`来为配置项结构体分配内存，这不光是用来存放配置项参数，还可以存放可能使用的变量的名字/索引

* 按照 HTTP 模块的顺序，依次调用其 preconfiguration 方法。这是定义变量的唯一机会。由于 ngx_http_core_module 是第一个 HTTP 模块，所以它的 preconfiguration 最先被调用，这个方法只做了一件事，就是调用`ngx_http_variables_add_core_vars`:

  ```c
  ngx_int_t
  ngx_http_variables_add_core_vars(ngx_conf_t *cf)
  {
    	// 初始化 cmcf->variables_keys cmcf->prefix_variables
  		...
      for (cv = ngx_http_core_variables; cv->name.len; cv++) {
          v = ngx_http_add_variable(cf, &cv->name, cv->flags);
          if (v == NULL) {
              return NGX_ERROR;
          }
          *v = *cv;
      }
    	return NGX_OK;
  }
  ```

  其实就是把的`ngx_http_core_variables`数组中的所有预设内部变量名(即`ngx_http_variable_t`)全部添加到`cmcf->variables_keys`中去。

* 然后解析`http{...}`配置块中的配置项，这个就是根据配置项的名名称来找到对应模块的解析方法来解析。如果有模块使用了变量，通常会在这一步骤把变量给索引化，这样的话只有使用到的变量才会被索引化，既加快了访问速度，也尽可能减少了不必要的内存分配。

* 然后开始调用各个 HTTP 模块的 postconfiguration 方法，这个时候配置项已经解析完了，也初始化完变量了，此时会决定模块如何接入到 HTTP 模块的 11 个阶段中：

  ```c
  static ngx_int_t
  ngx_http_mytest_postconfiguration(ngx_conf_t *cf)
  {
    	ngx_http_handler_pt				 *h;
    	ngx_http_core_main_conf_t  *cmcf;
    
    	cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    
    	h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    	if (h == NULL) {
        	return NGX_ERROR;
      }
    
    	*h = ngx_http_mytest_handler;
    
    	return NGX_OK;
  }
  ```

  比如说像上面，就是把 mytest 模块加入到`NGX_HTTP_ACCESS_PHASE`这个阶段中去了。

* 前面说了，是否将变量名索引化由使用它的模块决定，而不是由定义它的模块决定。这样就可能存在着问题，如果一个模块索引了一个变量，但是其实这个变量并没有被定义，或者说这个变量被其他某个模块定义了，但是却没有被编译进 Nginx。所以`ngx_http_variables_init_vars`其中一个作用就是解决这个问题。这个函数首先要求确保被索引了的变量都是合法的，也就是说他们必须已经被定义过了；其次，使用变量索引的模块只知道是根据某个变量名进行索引的，此时需要把相应的变量解析方法等熟悉也设置好(即加入到`cmcf->variables`数组中取)。

### 特殊内部变量

通常变量名是非常明确的，比如说在`ngx_http_core_variables`数组中预设的一些变量：

```c
static ngx_http_variable_t  ngx_http_core_variables[] = {
  	...
    { ngx_string("remote_addr"), NULL, ngx_http_variable_remote_addr, 0, 0, 0 },

    { ngx_string("remote_port"), NULL, ngx_http_variable_remote_port, 0, 0, 0 },

    { ngx_string("proxy_protocol_addr"), NULL,
      ngx_http_variable_proxy_protocol_addr,
      offsetof(ngx_proxy_protocol_t, src_addr), 0, 0 },
  	...
}
```

这几个变量名都是直接被硬编码在代码中的。但是还有其他一些变量：

* 它们的名称是未知的
* 但是如何解析它们却是一目了然的

比如说 HTTP 请求的 URL 中的变量，`/sitemap.xml?page_num=2`，我们希望得到`page_num`这个变量。这种变量有很多(数不清)，但是它们的解析方法是一致的，就是从 URL 根据变量名进行匹配。

这样的变量 Nginx 总结为 5 类，对于每一类，虽然说变量名各种各样，但是其实只需要一个解析方法。这 5 类变量由 HTTP 框架所定义，并且要求使用它们的模块必须给变量名加上对应的前缀`http_`, `sent_http_`，`sent_trailer_`，`cookie_`或者`arg_`：

| 变量前缀         | 含义                     | 解析方法                                |
| ---------------- | ------------------------ | --------------------------------------- |
| `http_`          | 请求中的 HTTP 头部       | `ngx_http_variable_unknown_header_in`   |
| `sent_http_`     | 响应中的 HTTP 头部       | `ngx_http_variable_unknown_header_out`  |
| `sent_trailer_`  | 后端服务器 HTTP 响应头部 | `ngx_http_upstream_unknown_trailer_out` |
| `cookie_`        | Cookie 头部中的某个项    | `ngx_http_variable_cookie`              |
| `arg_`           | 请求的 URL 中的参数      | `ngx_http_variable_argument`            |

这几类变量也和普通变量一样存在`ngx_http_core_variables`数组中，但是都加上了`NGX_HTTP_VAR_PREFIX`标志位：

```c
static ngx_http_variable_t  ngx_http_core_variables[] = {
  	...
    { ngx_string("http_"), NULL, ngx_http_variable_unknown_header_in,
      0, NGX_HTTP_VAR_PREFIX, 0 },

    { ngx_string("sent_http_"), NULL, ngx_http_variable_unknown_header_out,
      0, NGX_HTTP_VAR_PREFIX, 0 },

    { ngx_string("sent_trailer_"), NULL, ngx_http_variable_unknown_trailer_out,
      0, NGX_HTTP_VAR_PREFIX, 0 },

    { ngx_string("cookie_"), NULL, ngx_http_variable_cookie,
      0, NGX_HTTP_VAR_PREFIX, 0 },

    { ngx_string("arg_"), NULL, ngx_http_variable_argument,
      0, NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_PREFIX, 0 },

      ngx_http_null_variable
};

```

其中有个地方就是`arg_`类型的变量加了一个`NGX_HTTP_NOCACHEABLE`标志位，这意味着这种类型变量的值无法被缓存在`ngx_http_request_t::variables`数组中，每次获取其值，都只能通过其`get_handler`解析。TODO: 为什么一定要这个参数？

此外，需要注意的是，这 5 类变量并不能被加入到哈希表中取，因为此时(即定义时，或者说通过`ngx_http_variables_add_core_vars`添加变量时)我们还不知道它的变量名。但是我们可以将这几类变量索引化，因为索引化是由使用该变量的模块决定的，而使用变量时一定是已经知道了变量的名字了。用索引来获取这几类变量的值是最常用的方式。

## 如何在代码中使用内部变量

那我们在代码里面怎么使用变量呢？先看看这个 nginx.conf：

```nginx
http {
    server {
        listen       9877;
        server_name  localhost;

        location /myspace {
            allow_in $remote_addr 10.69.50.199;
        }
    }
}
```

我们自定义了一个配置项`allow_in`，只有对端 ip 为 10.69.50.199 时，才允许访问。里面用到了`$remote_addr`这个内部变量，它是 HTTP 框架预设的一个变量(即在`ngx_http_core_variables`数组中)。

```c
typedef struct {
    int        variable_index;
    ngx_str_t  variable;
    ngx_str_t  target;
} ngx_http_allow_in_loc_conf_t;


static ngx_command_t  ngx_http_testvariable_commands[] = {

        { ngx_string("allow_in"),
          NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
          ngx_http_allow_in,
          NGX_HTTP_LOC_CONF_OFFSET,
          0,
          NULL,
        },

        ngx_null_command,
};


static ngx_http_module_t  ngx_http_testvariable_module_ctx = {
        NULL,
        ngx_http_allow_in_init,
        NULL,
        NULL,
        NULL,
        NULL,
        ngx_http_allow_in_create_loc_conf,
        NULL
};


ngx_module_t  ngx_http_testvariable_module = {
        NGX_MODULE_V1,                             /* */
        &ngx_http_testvariable_module_ctx,         /* */
        ngx_http_testvariable_commands,
        NGX_HTTP_MODULE,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NGX_MODULE_V1_PADDING
};

static void *
ngx_http_allow_in_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_allow_in_loc_conf_t  *aicf;

    aicf = ngx_palloc(cf->pool, sizeof(ngx_http_allow_in_loc_conf_t));
    if (aicf == NULL) {
        return NULL;
    }

    aicf->variable_index = -1;

    return aicf;
}

static char *
ngx_http_allow_in(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                     *value;
    ngx_http_allow_in_loc_conf_t  *aicf;

    aicf = conf;
    value = cf->args->elts;

    if (cf->args->nelts != 3) {
        return NGX_CONF_ERROR;
    }

    if (value[1].data[0] != '$') {
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;
    aicf->variable_index = ngx_http_get_variable_index(cf, &value[1]);
    if (aicf->variable_index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }
    aicf->variable = value[1];
    aicf->target = value[2];

    return NGX_CONF_OK;
}

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

static ngx_int_t
ngx_http_allow_in_handler(ngx_http_request_t *r)
{
    ngx_http_variable_value_t     *vv;
    ngx_http_allow_in_loc_conf_t  *aicf;

    aicf = ngx_http_get_module_loc_conf(r, ngx_http_testvariable_module);
    if (aicf == NULL) {
        return NGX_ERROR;
    }

    if (aicf->variable_index == -1) {
        return NGX_DECLINED;
    }

    vv = ngx_http_get_indexed_variable(r, aicf->variable_index);
    if (vv == NULL || vv->not_found) {
        return NGX_HTTP_FORBIDDEN;
    }

    if (vv->len == aicf->target.len
        && ngx_strncmp(aicf->target.data, vv->data, vv->len) == 0)
    {
        return NGX_DECLINED;
    }

    return NGX_HTTP_FORBIDDEN;
}
```

上面就是一个 HTTP 模块的所有代码了(除了 include)，`allow_in $remote_addr  10.69.50.199`的意思是只有对端地址为 10.69.50.199 才能访问该 location。如果在配置文件中用到了该配置项，那么在解析该配置项时拿到`$remote_addr`这个变量的索引(即将其索引化)，然后在变量解析完之后的 postconfiguration 回调中把这个 handler 注册到 HTTP 请求的 11 个阶段的 access 阶段。当请求到达该 location，调用这个 handler，此时在这个 handler 中通过该索引拿到这个变量的值，和目标匹配以决定是否允许访问。

编译、运行：

```console
% http localhost:9877/myspace
HTTP/1.1 403 Forbidden
Connection: keep-alive
Content-Length: 153
Content-Type: text/html
Date: Wed, 29 Jan 2020 08:11:28 GMT
Server: nginx/1.17.6

<html>
<head><title>403 Forbidden</title></head>
<body>
<center><h1>403 Forbidden</h1></center>
<hr><center>nginx/1.17.6</center>
</body>
</html>
```

被禁止访问了，符合预期。

### Nginx 提供使用的一些方法



## 变量与配置项的区别

因为基本没有用过 nginx 中的变量(说基本是因为看 openresty 的 readme 时抄过一段 )，所以现在有种“既然有了配置项为什么还要变量”的想法，究其原因，就是不知道变量和配置项各自扮演的是什么角色，起到的是什么作用？

Nginx 的多样性很大程度上来自 nginx.conf 文件中的各式各样的配置项，这些配置项五花八门，风格各异，原因是他们都是由各自的 nginx 模块自定义的，并没有什么统一的标准。但是 变量和他们不一样。

## 总结

