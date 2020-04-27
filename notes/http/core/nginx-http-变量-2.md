# nginx HTTP 变量（二）

## 外部变量

内部变量则是在 C 代码中定义的，外部变量是在 nginx.conf 配置文件中声明的。它是用 ngx_http_rewrite_module 提供的`set`指令来创建的：

```nginx
location /test {
  	set $a "hello";
}
```

变量的值一定是一个字符串，但是并不是只能通过这种字面值来定义，还可以通过**变量插值**来定义：

```nginx
location /foo {
  	set $a "hello";
  	set $b "$a world";
  	set $c "${a}world";
}
```

在`rewrite`指令中还可以配合正则表达式：

```nginx
rewrite /download/(.*)/mp3 /music/$1.mp3;
```

再配合`if`等关键字，可以说是功能强大了。而且实现也很巧妙，来看看`set`指令的 handler：

## `set`指令源码剖析

在 nginx.conf 文件中我们经常可以看见用`set`指令定义的外部变量，所以拿他做例子会比较好一些：

```c
static char *
ngx_http_rewrite_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_rewrite_loc_conf_t  *lcf = conf;

    ngx_int_t                            index;
    ngx_str_t                           *value;
    ngx_http_variable_t                 *v;
    ngx_http_script_var_code_t          *vcode;
    ngx_http_script_var_handler_code_t  *vhcode;

    value = cf->args->elts;

    if (value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

  	value[1].len--;
  	value[1].data++;

```

前面一段很简单，就是确保变量名是以`$`开头的。

```c
    v = ngx_http_add_variable(cf, &value[1],
                              NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_WEAK);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }


		index = ngx_http_get_variable_index(cf, &value[1]);
    if (index == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }
```

然后就是把变量名添加到`cmcf->variables_keys`中，并将其索引化(添加到`cmcf->variables`数组中)。

```c
    if (v->get_handler == NULL) {
        v->get_handler = ngx_http_rewrite_var;
        v->data = index;
    }
```

上面这一段代码时为了处理特殊情况的。我们知道内部变量的`get_handler`是一定要实现的，因为通常都是采用**惰性求值**，也就是只有读取变量值时才会通过`get_handler`来计算出这个值。但是外部变量是不同的，每一次`set`都会给变量重新赋值，而且由于外部变量是索引化和缓存的的，所以可以直接从请求的`variables`数组中取到`set`之后的变量值。这样看来`get_handler`是多余的，但是我们知道变量一旦创建，其变量名的可见范围就是整个配置，所以虽然我们可能在一个地方对变量使用了`set`，但是在其他地方虽然没有`set`，但是我们还是可以使用这个变量，此时由于变量的生命期不可跨越请求(也就是说每个请求持有一个变量的副本)，所以我们得确保在其他地方可以拿到这个外部变量的一个默认值：

```nginx
server {
  	listen   		 80;
  	server_name  localhost;
  
  	location /foo {
    		echo "a = [$a]";
  	}
  
  	location /bar {
    		set $a "hello";
    		echo "a = [$a]";
  	}
}
```

上面在 /bar 这个 location 我们使用`set`指令定义了`$a`这个变量，在解析配置阶段就把变量名加入了`cmcf->variables`中了，所以变量名在 /foo 这个 location 中是可见的，当我们`curl http://localhost/foo`请求 /foo 这个 location 时，会输出`a = []`，一个空字符串。这就是`ngx_http_rewrite_var`函数的返回值。由于在请求 /foo 时(TODO: 那么`ngx_http_rewrite_var`是在什么时候调用的呢？在请求 /foo 时我们变量名已经索引化了，所以我们不是应该根据其下标值从请求结构体的`variables`数组中取么？)

## 脚本引擎

我们知道调用`set`的 handler 这些步骤都是处于配置文件解析的状态，也就是在`ngx_conf_parse`时遇到了`set`指令，就调用其 handler。但是 nginx 为了不浪费资源，选择把这些指令的变量名和变量值都封装成一个一个易于执行的实体，而并未真正地解析其值，直到有请求到来才真正地把这些值给解析出来。或者说 nginx 不得不这样做，因为有的变量值只有在请求到来时才能真正确定，比如：

```nginx
location /mp3 {
  	set $a "$arg_author/${arg_song}.mp3"
}
```

只有当真正请求了 /mp3 这个 location 时，我们才能够在 URI 参数中拿到 author 和 song 这两个参数的值，然后拼接(变量插值)作为`$a`这个变量的值。nginx 选择把变量名和变量值都封装成一个一个的`xxx_code_t`的结构体，待请求到来时执行里面的回调函数，真正地获取到变量的值。

同一段脚本被编译进 Nginx，在不同的请求里执行是的效果是完全不同的，所以每一个请求都必须有其独有的脚本执行上下文，或者称为脚本引擎。这是整个外部变量脚本执行的最关键的数据结构：

```c
typedef struct {
    u_char                     *ip;
    u_char                     *pos;
    ngx_http_variable_value_t  *sp;

    ngx_str_t                   buf;
    ngx_str_t                   line;

    /* the start of the rewritten arguments */
    u_char                     *args;

    unsigned                    flushed:1;
    unsigned                    skip:1;
    unsigned                    quote:1;
    unsigned                    is_args:1;
    unsigned                    log:1;

    ngx_int_t                   status;
    ngx_http_request_t         *request;
} ngx_http_script_engine_t;

```

解释一下其中的一些重要字段：

* `ip`：指向当前正在执行的 code
* `sp`：栈
* `request`：当前脚本引擎所属的 HTTP 请求

然后就是 ngx_http_rewrite_module 模块的 location 级别的配置结构体了，里面存储着该 location 下在解析配置文件阶段编译出来的所有 code 以及其他一些配置指令的值。

```c
typedef struct {
    ngx_array_t  *codes;        /* uintptr_t */

    ngx_uint_t    stack_size;

    ngx_flag_t    log;
    ngx_flag_t    uninitialized_variable_warn;
} ngx_http_rewrite_loc_conf_t;
```

* `codes`: 存储该 location 的脚本编译出来的所有的 code
* `stack_size`：即`ngx_http_script_engine_t`中栈(`sp`)的大小
* `uninitialized_variable_warn`：在使用未赋值的变量时，是否在 log 中打告警日志。这个是在前面提到的`ngx_http_rewrite_var`函数中使用的。

这里需要注意的是

```c
static ngx_int_t
ngx_http_rewrite_handler(ngx_http_request_t *r)
{
		...
      
		e = ngx_pcalloc(r->pool, sizeof(ngx_http_script_engine_t));
    if (e == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    e->sp = ngx_pcalloc(r->pool,
                        rlcf->stack_size * sizeof(ngx_http_variable_value_t));
    if (e->sp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    e->ip = rlcf->codes->elts;
    e->request = r;
    e->quote = 1;
    e->log = rlcf->log;
    e->status = NGX_DECLINED;

    while (*(uintptr_t *) e->ip) {
        code = *(ngx_http_script_code_pt *) e->ip;
        code(e);
    }

    return e->status;
}

```

看`e->ip = rlcf->codes->elts`和`code = *(ngx_http_script_code_pt *) e->ip`这两句。这里有两个问题：

1. `ngx_http_script_engine_t::ip`成员是一个`u_char`类型的指针，而其实`ngx_http_rewrite_loc_conf_t::codes`数组中存储的是`ngx_xxx_code_t`类型的元素，为什么要这样呢？
2. 为什么对于每个 code 都可以把把`e->ip`强制转换为`ngx_http_script_code_pt *`？更进一步，为什么要这样做呢，这个回调有什么特殊的么？

首先解答第一个问题，我们已经知道了`ngx_http_rewrite_loc_conf_t::codes`数组中存储的是`ngx_xxx_code_t`类型的值。而这一类 code 的类型是不一样的，意味着大小也是不一样的，而`ngx_array_t`结构要求元素大小是相同的，怎么办呢？Nginx 用了一个非常巧妙的技巧，就是把 codes 数组元素大小设置为 1 个字节，每次要存`ngx_xxx_code_t`类型的值是，就调用`ngx_http_script_start_code`，按照该 code 的大小在 codes 数组中分配适量内存：

```c
void *
ngx_http_script_start_code(ngx_pool_t *pool, ngx_array_t **codes, size_t size)
{
    if (*codes == NULL) {
        *codes = ngx_array_create(pool, 256, 1);
        if (*codes == NULL) {
            return NULL;
        }
    }

    return ngx_array_push_n(*codes, size);
}

```

比如：

```c
        val = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                         sizeof(ngx_http_script_value_code_t));
```

所以我们知道其实`ngx_http_rewrite_loc_conf_t::codes`中存储的其实还是`ngx_xxx_code_t`类型的值，只不过因为这些 code 的大小不一才把数组元素大小设置为 1。

然后是第二个问题，在上面`ngx_http_rewrite_handler`函数的`while`循环中，我们并没有看到对`e->ip`的更新操作，很奇怪，执行完一个 code 不是应该更新 ip 执行下一个 code 么？其实这个操作根本无法由 code 的使用者完成，因为每个 code 的大小是不一样的，所以只能由 code 自己来自增。这里 nginx 又使用了 C 语言的一个特性，即**结构体的地址和它的首元素的地址是相同的**，所以当拿到一个 code 的指针时，可以把这个指针转换为 code 结构体内首元素类型的指针。而 nginx 则规定每个 code 的首元素都是一个`ngx_http_script_code_pt`类型的指针：

```c
typedef void (*ngx_http_script_code_pt) (ngx_http_script_engine_t *e);
```

比如用来表示`set`指令创建的变量的变量名的 code：

```c
static char *
ngx_http_rewrite_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
		...
      
		vcode = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                       sizeof(ngx_http_script_var_code_t));
    if (vcode == NULL) {
        return NGX_CONF_ERROR;
    }

    vcode->code = ngx_http_script_set_var_code;
    vcode->index = (uintptr_t) index;
  	...
```

当然这只是变量名 code 的一种情况。来看看`ngx_http_script_var_code_t`和`ngx_http_script_set_var_code`函数：

```c
typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   index;
} ngx_http_script_var_code_t;

void
ngx_http_script_set_var_code(ngx_http_script_engine_t *e)
{
    ngx_http_request_t          *r;
    ngx_http_script_var_code_t  *code;

    code = (ngx_http_script_var_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_code_t);

    r = e->request;

    e->sp--;

    r->variables[code->index].len = e->sp->len;
    r->variables[code->index].valid = 1;
    r->variables[code->index].no_cacheable = 0;
    r->variables[code->index].not_found = 0;
    r->variables[code->index].data = e->sp->data;
}
```

注意里面的`e->ip += sizeof(ngx_http_script_var_code_t)`，果不其然，ip 的递增是在`ngx_http_script_code_pt`回调中执行的，而且只能在这个回调里面执行，而不能由在外部执行。

还有最后一个问题，`ngx_http_script_engine_t::sp`这个字段是用来做什么呢？前面我们只是知道了各个字段的作用，但是对于为什么要把变量和变量名都编译成 code 以及请求到来时变量名和变量值在 code 的情况下如何交互，以及处理一条诸如`set $a "hello";`这样的指令的整体流程还是不清楚。而这个`sp`就是关键。

还是举`set $a hello;`这个例子，变量名和变量值分别被编译为`ngx_http_script_set_var_code`和`ngx_http_sript_value_code`：

```c
typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   index;
} ngx_http_script_var_code_t;

// code 字段为 ngx_http_script_set_var_code
void
ngx_http_script_set_var_code(ngx_http_script_engine_t *e)
{
    ngx_http_request_t          *r;
    ngx_http_script_var_code_t  *code;

    code = (ngx_http_script_var_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_var_code_t);

    r = e->request;

    e->sp--;

    r->variables[code->index].len = e->sp->len;
    r->variables[code->index].valid = 1;
    r->variables[code->index].no_cacheable = 0;
    r->variables[code->index].not_found = 0;
    r->variables[code->index].data = e->sp->data;
}



typedef struct {
    ngx_http_script_code_pt     code;
    uintptr_t                   value;
    uintptr_t                   text_len;
    uintptr_t                   text_data;
} ngx_http_script_value_code_t;

// code 字段为 ngx_http_script_value_code
void
ngx_http_script_value_code(ngx_http_script_engine_t *e)
{
    ngx_http_script_value_code_t  *code;

    code = (ngx_http_script_value_code_t *) e->ip;

    e->ip += sizeof(ngx_http_script_value_code_t);

    e->sp->len = code->text_len;
    e->sp->data = (u_char *) code->text_data;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, e->request->connection->log, 0,
                   "http script value: \"%v\"", e->sp);

    e->sp++;
}
```

在`set`指令的`ngx_http_rewrite_set`这个 handler 我们可以看到是先编译 value 存入codes 数组，然后再编译 var。看似有点违反直觉，不应该从左到右依次编译 var 然后是 value 么？看看`sp`在二者之间的作用就知道为什么要这么做了。

首先看 value 的，更新 ip，前面解释了，ip 就应该在`ngx_http_script_code_pt`里面更新。然后把变量的值放在栈顶，随后更新栈顶。

然后是 var 的，递减栈顶，此时就到了前一步 value 设置的 code 的位置了，此时把数据取下来，根据 var 的 code 里面保存的变量名的下标把变量值更新到请求结构体的`variables`数组中去。

所以看到这里应该就看明白了 sp 的作用了。var 和 value 被编译成了两个 code，var 这个 code 里面保存有这个变量名的下标索引，而 value 这个 code 这保存有变量的值，但是`set`指令要求用 var 里面的下标索引从`ngx_http_request::variables`数组中找到变量值的存储位置，然后用 value 里面的变量值更新。所以 sp 这个栈起的是一个数据传递的作用。

### 将 var 和 value 都编译为 code

继续看`ngx_http_rewrite_set`函数：

```c
    if (ngx_http_rewrite_value(cf, lcf, &value[2]) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }
```

然后是将 var 给 code 化：

```c
    if (v->set_handler) {
        vhcode = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                   sizeof(ngx_http_script_var_handler_code_t));
        if (vhcode == NULL) {
            return NGX_CONF_ERROR;
        }

        vhcode->code = ngx_http_script_var_set_handler_code;
        vhcode->handler = v->set_handler;
        vhcode->data = v->data;

        return NGX_CONF_OK;
    }

    vcode = ngx_http_script_start_code(cf->pool, &lcf->codes,
                                       sizeof(ngx_http_script_var_code_t));
    if (vcode == NULL) {
        return NGX_CONF_ERROR;
    }

    vcode->code = ngx_http_script_set_var_code;
    vcode->index = (uintptr_t) index;

    return NGX_CONF_OK;
}

```

这里首先判断变量名结构体的`set_handler`是否为空，如果不为空则用`ngx_http_script_var_handler_code_t`保存变量名，否则用`ngx_http_script_var_code_t`保存变量名。为什么要这样呢？设置了`set_handler`的变量有什么特殊之处呢？其实这是在处理内建变量和外部变量混用的问题：

1. 大部分情况下，内部变量不会与外部变量混用。此时，我们把`ngx_http_script_var_code_t`指令结构体添加到codes 数组中去，再把变量的索引传到其 index 成员，并设置变量指定的执行方法`ngx_http_script_var_code`这是`if`后面的代码所做的事情
2. 如果一个内部变量希望在 nginx.conf 配置文件中用 set 修改其值，那么它就会
实现 set_handler 方法。意思在这个 set_handler 里面会执行变量值的修改操作，而不用在 var 的`ngx_http_script_code_pt`回调中手动更新。

那么为什么要对`set $built_in xxx`这种对内部变量赋值的情况特殊对待呢？原因有几点：

1. 外部变量，其值是被缓存在`ngx_http_reqeust_t::variables`数组中的，我们取值的时候只能通过下标从这个数组中拿；更新值的时候，也必须通过下标来更新这个数组中对应的元素，这个就是`ngx_http`的事情，外部变量的设置方法是通用的，所以可以使用这一个函数来完成所有外部变量的设置工作。
2. 但是内部变量不一样，并不是所有的内部变量都被缓存在请求的`variables`数组中，比如说 limit_rate 这个内部变量，其值直接存储在 ngx_http_request_t::limit_rate 中，所以 ngx_http_script_var_code 并不能处理这些情况，所以新创建了`ngx_http_script_var_handler_code_t` 这个结构，多加两个以 handler 方法用来一对一地进行处理。
3. 在 ngx_http_variables.c 中有 args 和 limit_rate 两个内部变量设置了`set_handler`，可以参考一下。

## 参考
