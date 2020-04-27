# 配置文件的解析

## 源码剖析

配置文件解析代码都在`src/core/ngx_conf_file.h/c`中，

```c
char *
ngx_conf_parse(ngx_conf_t *cf, ngx_str_t *filename)
{
    char             *rv;
    ngx_fd_t          fd;
    ngx_int_t         rc;
    ngx_buf_t         buf;
    ngx_conf_file_t  *prev, conf_file;
    enum {
        parse_file = 0,
        parse_block,
        parse_param
    } type;
    
```

在函数内部定义了`type`这个枚举类型，表示正在解析的位置。

### 确定解析类型

```c
#if (NGX_SUPPRESS_WARN)
    fd = NGX_INVALID_FILE;
    prev = NULL;
#endif

    if (filename) {
        fd = ngx_open_file(filename->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
        
        if (fd == NGX_INVALID_FILE) {
            ...
            return NGX_CONF_ERROR;
        }
        
        prev = cf->conf_file;
        
        cf->conf_file = &conf_file;
        
        if (ngx_fd_info(fd, &cf->conf_file->file.info) == NGX_FILE_ERROR) {
            ...
            return NGX_CONF_ERROR;
        }
        
        cf->conf_file->buffer = &buf;
        
        buf.start = ngx_alloc(NGX_CONF_BUFFER, cf->log);
        if (buf.start == NULL) {
            goto failed;
        }
        
        buf.pos = buf.start;
        buf.last = buf.start;
        buf.end = buf.last + NGX_CONF_BUFFER;
        buf.temporary = 1;
        
        cf->conf_file->file.fd = fd;
        cf->conf_file->file.name.len = filename->len;
        cf->conf_file->file.name.data = filename->data;
        cf->conf_file->file.offset = 0;
        cf->conf_file->file.log = cf->log;
        cf->conf_file->line = 1;

        type = parse_file;

    } else if (fd != NGX_INVALID_FILE) {
        type = parse_block;

    } else {
        type = parse_param;
    }
```

首先确定解析类型。

如果传入的`filename`参数不为`NULL`，那么说明是第一次解析，所以首先需要打开文件，
并创建缓冲区用于存储文件数据。需要注意的是`prev`变量，这个变量用来做备份用。

### 读取文件并解析

前面确定了解析类型，这里就开始读取文件内容并进行解析了。

```c
    for ( ;; ) {
        rc = ngx_conf_read_token(cf);

        /*
         * ngx_conf_read_token() may return
         *
         *    NGX_ERROR             there is error
         *    NGX_OK                the token terminated by ";" was found
         *    NGX_CONF_BLOCK_START  the token terminated by "{" was found
         *    NGX_CONF_BLOCK_DONE   the "}" was found
         *    NGX_CONF_FILE_DONE    the configuration file is done
         */
         
         if (rc == NGX_ERROR) {
             goto done;
         }
         
         if (rc == NGX_CONF_BLOCK_DONE) {

             if (type != parse_block) {
                 ...
                 goto failed;
             }
             
             goto done;
         }
         
         if (rc == NGX_CONF_FILE_DOEN) {
         
             if (type == parse_block) {
                  ...
                  goto failed;
             }
             
             goto done;
         }
         
         if (rc == NGX_CONF_BLOCK_START) {

             if (type == parse_param) {
                 ...
                 goto failed;
             }
         }
         
```

在一个无线循环中读取 token，读取到了之后首先进行合法性检查。比如在`parse_block`
阶段如果返回的是文件读取完毕(`NGX_CONF_FILE_DONE`)，那么说明出错了，需要处理。

```c
         /* rc == NGX_OK || rc == NGX_CONF_BLOCK_START */

         if (cf->handler) {
         
         }
         
         rc = ngx_conf_handler(cf, rc);
         
         if (rc == NGX_ERROR) {
             goto failed;
         }
    }
```

#### `ngx_conf_handler`和配置项解析方法挂钩

可以看到在最后面调用了`ngx_conf_handler`，这个方法是来调用对应配置项的的解析方法
的，我们知道各个模块在构建自己的`ngx_command_t`时都是要注册该配置项的解析方法的。

```c
static ngx_int_t
ngx_conf_handler(ngx_conf_t *cf, ngx_int_t last)
{
    char           *rv;
    void           *conf, **confp;
    ngx_uint_t      i, found;
    ngx_str_t      *name;
    ngx_command_t  *cmd;
}
```

##### 1. 首先找到对应的`ngx_command_t`

```c
    for (i = 0; cf->cycle->modules[i]; i++) {
    
        cmd = cf->cycle->modules[i]->commands;
        if (cmd == NULL) {
            continue;
        }
        
        for (/* void */; cmd->name.len; cmd++) {

            if (name->len != cmd->len) {
                continue;
            }
            
            if (ngx_strcmp(name->data, cmd->data) != 0) {
                continue;
            }
            
            found = 1;
```

寻找的方法很直接，就是遍历所有模块的`ngx_command_t`数组中的所有指令，没有用什么
奇技淫巧，但是用了一些方法避免无谓的搜索，比如比较指令长度。

##### 2. 检查该配置项是否有效

1. 首先类型检查，解析出来的

``` C
            if (cf->cycle->modules[i]->type != NGX_CONF_MODULE
                && cf->cycle->modules[i]->type != cf->module_type)
            {
                continue;
            }
            
            if (!(cmd->type & cf->cmd_type)) {
                continue;
            }

            if (!(cmd->type & NGX_CONF_BLOCK) && last != NGX_OK) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                  "directive \"%s\" is not terminated by \";\"",
                                  name->data);
                return NGX_ERROR;
            }

            if ((cmd->type & NGX_CONF_BLOCK) && last != NGX_CONF_BLOCK_START) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "directive \"%s\" has no opening \"{\"",
                                   name->data);
                return NGX_ERROR;
            }

```

2. 然后是参数检查。

我们知道指令的参数也是它类型的一部分，我们在`ngx_command_t`结构中的 type 字段中
来指定该指令后面可以带几个参数，比如`NGX_CONF_TAKE1`, `NGX_CONF_ANY`, 
`NGX_CONF_FLAG`，我们需要确保参数是对的上的。

```c
            if (!(cmd->type & NGX_CONF_ANY)) {

                if (cmd->type & NGX_CONF_FLAG) {

                    if (cf->args->nelts != 2) {
                        goto invalid;
                    }

                } else if (cmd->type & NGX_CONF_1MORE) {

                    if (cf->args->nelts < 2) {
                        goto invalid;
                    }

                } else if (cmd->type & NGX_CONF_2MORE) {

                    if (cf->args->nelts < 3) {
                        goto invalid;
                    }

                } else if (cf->args->nelts > NGX_CONF_MAX_ARGS) {

                    goto invalid;

                } else if (!(cmd->type & argument_number[cf->args->nelts - 1]))
                {
                    goto invalid;
                }
            }
```

##### 3. 将配置项参数写入到其`ctx`中去

每个模块都有自己的配置项结构体，各个模块会在 Nginx 框架的调度下创建这些配置项结
构体的指针，然后将其存入`ngx_cycle::conf_ctx`数组中。在进行配置文件的解析时，我们
真正需要做的其实就是把这些配置项的

```c
            /* set up the directive's configuration context */

            conf = NULL;

            if (cmd->type & NGX_DIRECT_CONF) {
                conf = ((void **) cf->ctx)[cf->cycle->modules[i]->index];

            } else if (cmd->type & NGX_MAIN_CONF) {
                conf = &(((void **) cf->ctx)[cf->cycle->modules[i]->index]);

            } else if (cf->ctx) {
                confp = *(void **) ((char *) cf->ctx + cmd->conf);

                if (confp) {
                    conf = confp[cf->cycle->modules[i]->ctx_index];
                }
            }

            rv = cmd->set(cf, cmd, conf);

            if (rv == NGX_CONF_OK) {
                return NGX_OK;
            }

            if (rv == NGX_CONF_ERROR) {
                return NGX_ERROR;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "\"%s\" directive %s", name->data, rv);

            return NGX_ERROR;
        }
    }
```

这个地方是疑问最大的地方了。为什么对于`ngx_command_t`根据其`type`来决定传递给
`set`函数的参数呢？怎么理解呢？

首先要注意，虽然`conf`是一个`void *`，但是他在上面的`if-else-if`语句块中却充当不
同的类型：

### `NGX_MAIN_CONF`类型的指令上下文的设置

当`type`中含有`NGX_MAIN_CONF`(说**含有** 而不说**是**是因为`type`字段还可以放其
它标志，比如带的参数的个数)时，`conf`被设置成
`((void **) cf->ctx)[cf->cycle->modules[i]->index]`，为什么要这样设置呢？来看一
个带有`NGX_MAIN_CONF`的指令`http`它的`set`方法吧：

```c
static ngx_command_t  ngx_http_commands[] = {

    { ngx_string("http"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_http_block,
      0,
      0,
      NULL },

      ngx_null_command
};

static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                        *rv;
    ngx_uint_t                   mi, m, s;
    ngx_conf_t                   pcf;
    ngx_http_module_t           *module;
    ngx_http_conf_ctx_t         *ctx;
    ngx_http_core_loc_conf_t    *clcf;
    ngx_http_core_srv_conf_t   **cscfp;
    ngx_http_core_main_conf_t   *cmcf;

    if (*(ngx_http_conf_ctx_t **) conf) {
        return "is duplicate";
    }

    /* the main http context */

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *(ngx_http_conf_ctx_t **) conf = ctx
}
```

首先注意`*(ngx_http_conf_ctx_t **) conf = ctx`这一句，根据`ngx_conf_handler`里面
的`if-elseif`判断，我们可以知道`conf`是`&(cf->ctx[ngx_http_module.index])`，这里
就需要知道此时`cf->ctx`里面是什么了。

由于`http`这条指令属于`ngx_http_module`，是一个`NGX_CORE_MODULE`类型的模块，这种
类型的模块的配置文件解析是在`ngx_init_cycle`函数中进行的：

```c
ngx_cycle_t *
ngx_init_cycle(ngx_cycle_t *old_cycle)
{
    ngx_conf_t           conf
    
    ngx_memzero(&conf, sizeof(ngx_conf_t));
    
    conf.ctx = cycle->conf_ctx;
    conf.module_type = NGX_CORE_MODULE;
    conf.cmd_type = NGX_MAIN_CONF;
    
    if (ngx_conf_parse(&conf, &cycle->conf_file) != NGX_CONF_OK) {
        ...
        return NULL;
    }
    
    ...
}
```

为了抓住重点，代码中我忽略了与`conf->ctx`无关的部分。从里面可以看到其实在
`ngx_conf_handler`中`cf->ctx`其实就是`ngx_cycle_t::conf_ctx`，所以在设置`http`指
令的上下文时，传递给`ngx_http_block`函数的`conf`参数的实参实际上就是
`&(ngx_cycle_t::conf_ctx[ngx_http_module.index])`，然后在`ngx_http_block`函数内部
通过`*(ngx_http_conf_ctx_t **) conf = ctx`将其设置为了属于`ngx_http_module`的配
置项结构体:

```c
typedef struct {
    void        **main_conf;
    void        **srv_conf;
    void        **loc_conf;
} ngx_http_conf_ctx_t;
```

### 非`NGX_DIRECT_CONF`和`NGX_MAIN_CONF`的配置项的上下文的设置

前面看了`http`指令的上下文设置，它是`NGX_MAIN_CONF`类型的，下面来看看`server`指
令，它是`NGX_HTTP_MAIN_CONF`类型的， 不是`NGX_DIRECT_CONF`，也不是`NGX_MAIN_CONF`。

```c
static ngx_command_t  ngx_http_core_commands[] = {

    ...

    { ngx_string("server"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_http_core_server,
      0,
      0,
      NULL },
      
    ...

    ngx_null_command,
}

static char *
ngx_http_core_server(ngx_conf_t *cf, ngx_command_t *cmd, void *dummy)
{
    ngx_http_conf_ctx_t         *ctx, *http_ctx;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    http_ctx = cf->ctx
    ctx->main_conf = http_ctx->main_conf;
    
    pcf = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_HTTP_SRV_CONF;

    rv = ngx_conf_parse(cf, NULL);
    
    ...
}
```

在`ngx_conf_handler`函数中我们可以看到，这种类型的`ngx_command_t`，传递给它的
`set`方法的`conf`参数是:

```c
            } else if (cf->ctx)
                confp = *(void **) ((char *) cf->ctx + cmd->conf);

                if (confp) {
                    conf = confp[cf->cycle->modules[i]->ctx_index];
                }
            }
```

首先看`confp`是什么，在这之前还是得看看`cf->ctx`是什么，这个字段不同模块的解析过
程中都是不同的，比如`server`指令，是属于`http{}`配置块的，是在`ngx_http_block`中
被解析的：

```c
static char *
ngx_http_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_conf_ctx_t         *ctx;
    
    if (*(ngx_http_conf_ctx_t **) conf) {
        return "is duplicate";
    }

    /* the main http context */

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *(ngx_http_conf_ctx_t **) conf = ctx;

    ...

    pcf = *cf;
    cf->ctx = ctx

    /* parse inside the http{} block */

    cf->module_type = NGX_HTTP_MODULE;
    cf->cmd_type = NGX_HTTP_MAIN_CONF;
    rv = ngx_conf_parse(cf, NULL);

    if (rv != NGX_CONF_OK) {
        goto failed;
    }
    
    ...
}
```

从上面的代码我们可以知道，在解析`http{}`配置块下的指令时，传递给`ngx_conf_handler`
的`cf->ctx`是`ngx_http_module`用来存储所有 HTTP 模块的`ngx_http_conf_ctx_t`结构体，
然后在`ngx_conf_handler`中，`conf`根据`cmd->conf`被设置成了指向`ngx_http_conf_ctx_t`
结构体中的`srv_conf[ngx_http_core_module.ctx_index]`。


这个`conf`被传给`server`指令的`set`方法(也就是`ngx_http_core_server`)，所有`server{}`
下的配置项都会在其中被创建、初始化。

### 错误处理

解析过程中可能会出错，此时就需要进行处理了。

```c
failed:
    rc == NGX_ERROR;
    
done:
    if (filename) {

        if (cf->conf_file->buffer.start) {
            ngx_free(cf->conf_file->buffer.start);
        }

        if (ngx_close_file(fd) == NGX_ERROR) {
            ...
            rc == NGX_ERROR;
        }
        
        cf->conf_file = prev;
    }
    
    if (rc == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }
    
    return NGX_CONF_OK;
}
```

## token 的读取

感觉这个部分应该不是特别重要，但是由于`ngx_conf_parse`函数并没有解答我的疑问，所
以我还是继续看下去了。

```c
```

## 总结

## 参考

[Nginx 配置文件解析详解](https://blog.csdn.net/u013510614/article/details/51818775)

[图解 Nginx 中的四级指针](http://blog.chinaunix.net/uid-27767798-id-3840094.html)
