# nginx mp4 模块解析(1)

## mp4 文件结构

有文件读取和缓冲区有关的字段大概有下面这几个：

```c
typedef struct {
    ngx_file_t file;
    u_char *buffer;
    u_char *buffer_start;
    u_char *buffer_pos;
    u_char *buffer_end;
    size_t buffer_size;

    off_t offset;
    off_t end;
    ...
} ngx_http_mp4_file_t;
```

## 读取流程

### `nginx_http_mp4_read`函数

```c
static ngx_int_t
ngx_http_mp4_read(ngx_http_mp4_file_t *mp4, size_t size)
{
    ssize_t n;
```

* 首先检查所需读取的数据(`size`长度)是否已经在缓冲区了:

```c
    if (mp4->buffer_pos + size <= mp4->buffer_end) {
        return NGX_OK;
    }
```

* 数据不在缓冲区，那么就需要从文件中读取数据到缓冲区

从文件读取数据，NGINX 的 mp4 模块每次都必须读满整个缓冲区(这样方便后面检查 `read` 是否完整)，如果文件中剩余(未读)的数据不够读满整个缓冲区，那么就要修改缓冲区大小(`mp4->buffer_size`)

```c
    if (mp4->offset + (off_t) mp4->buffer_size > mp4->end) {
        mp4->buffer_size = (size_t) (mp4->end - mp4->offset);
    }
```


还有可能缓冲区的大小不够读取所需要的数据(`size`参数指定了要读多少数据)，有两种情况会导致这个问题:
    * 分配的缓冲区本身不够大，导致不够读(nginx.conf文件中设置的`buffer_size`值太小了)
    * 缓冲区本身够大，但是文件中剩余的数据不够，而这经过前面的`mp4->buffer_size`大小调整之后又表现为`mp4->buffer_size`的值太小。

所以两种情况都可以通过`mp4->buffer_size`字段的值来判断

```c
    if (mp4->buffer_size < size) {
        ngx_log_error(NGX_LOG_ERR, mp4->file.log, 0,
                      "\"%s\" mp4 file truncated", mp4->file.name.data);
        return NGX_ERROR;
    }
```

* 检查缓冲区是否已经分配

前面都不设计真正的读写，只是对缓冲区大小进行了检查/调整。真正读取文件中的数据到缓冲区之前还需要检查缓冲区是否已经分配(第一次读时缓冲区就没有分配)。

```c
    if (mp4->buffer == NULL) {
        mp4->buffer = ngx_palloc(mp4->request->pool, mp4->buffer_size);
        if (mp4->buffer == NULL) {
            return NGX_ERROR;
        }

        mp4->buffer_start = mp4->buffer;
    }
```

* 真正地数据读取

```c
    n = ngx_read_file(&mp4->file, mp4->buffer_start, mp4->buffer_size,
                      mp4->offset);

    if (n == NGX_ERROR) {
        return NGX_ERROR;
    }

```

从文件读取完数据之后，需要检查是否读完整了。前面已经根据文件剩余大小和

```c
    if ((size_t) n != mp4->buffer_size) {
        ngx_log_error(NGX_LOG_CRIT, mp4->file.log, 0,
                     ngx_read_file_n " read only %z of %z from \"%s\"",
                    n, mp4->buffer_size, mp4->file.name.data);
        return NGX_ERROR;
    }
```

* 更新位置信息

```c
    mp4->buffer_pos = mp4->buffer_start;
    mp4->buffer_end = mp4->buffer_start + mp4->buffer_size;

    return NGX_OK;
}
```

#### 有关于读的一些问题

在`ngx_http_mp4_file_t`结构中有关文件读取的几个字段都在前面列出来了。大部分意思都很明确。
但是有一个字段`mp4->offset`字段困扰了我很久。一开始一直没有搞懂它和`mp4->buffer_start`，`mp4->buffer_pos`等几个缓冲区字段的关系。`ngx_http_mp4_read`函数，开始我总有一个问题:

> 如果缓冲区中还有数据没有被"消费"，但是又不够下一次读取，就需要从文件中读取。但是在`ngx_http_mp4_read`函数中使用`ngx_read_file`函数从文件读取时总是读取到`mp4->buffer_start`处，而不是读取到`mp4->buffer_pos`处，这不会造成原有的未被消费的数据被覆盖么？

这个问题困扰了我很久(其实中间有一段时间是知道为什么的，但是没有记笔记就又忘了为什么了)，其实问题就在在`mp4->offset`字段和`ngx_read_file`函数的最后一个参数上。

`ngx_read_file`函数其实是使用了`pread`函数(系统不支持的话也可以使用`read`和`lseek`函数一同实现)来从文件中真正地读取数据。这个函数比传统的`read`函数多了一个`offset`参数，用于在指定文件偏移量处读取数据。而`mp4->offset`就是`mp4->buffer_pos`对应在文件中的偏移量。所以从`mp4->offset`处开始读取数据到`mp4->buffer_start`相当于是把未处理的移动到了缓冲区的开头。其实是可以先使用`memmove`，然后在读取`size - tobeconsumed`数据，但是 nginx 的 mp4 模块这样做却一举两得(虽然把未消费的数据重复读了一遍)(不过我还是想说，真的是很厉害，读取流程我觉得是最重要的一块了，在对 flv 进行解析的时候我在文件读取上面卡了好久好久，要是早看懂 mp4 模块的就好了)

### `ngx_http_mp4_read_atom`函数

这个函数就是读取并解析所有 atom 的一个包装函数。在这个函数中它读取 atom，然后调用 每个 atom 对应的解析函数来对该 atom 进行解析。

```c
static ngx_int_t
ngx_http_mp4_read_atom(ngx_http_mp4_file_t *mp4,
    ngx_http_mp4_atom_handler *atom, uint64_t atom_data_size)
{
    off_t end;
    size_t atom_header_size;
    u_char *atom_header, *atom_name;
    uint64_t atom_size;
    ngx_int_t rc;
    ngx_uint_t n;

    end = mp4->offset + atom_data_size;
```

对于一个 atom，首先读取其 header，header 包括3个部分:

* size: 32位，表示 atom 的长度(包括 header)
* name(或者说 type): 表示该 atom 的名字，比如 moov，mdat 等

首先读取`size`字段:

```c
    while (mp4->offset < end) {
        if (ngx_http_mp4_read(mp4, sizeof(uint32_t)) != NGX_OK) {
            return NGX_ERROR;
        }

        atom_header = mp4->buffer_pos;
        atom_size = ngx_mp4_get32_value(atom_header);
        atom_header_size = sizeof(ngx_mp4_atom_header_t);

        if (atom_size == 0) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, mp4->file.log, 0,
                           "mp4 atom end")
            return NGX_OK;
        }

        if (atom_size < sizeof(ngx_mp4_atom_header_t)) {
            if (atom_size == 1) {
                if (ngx_http_mp4_read(mp4, sizeof(ngx_mp4_atom_header64_t))
                    != NGX_OK)
                {
                    return NGX_ERROR;
                }

                atom_header = mp4->buffer_pos;
                atom_size = ngx_mp4_get64value(atom_header + 8);
                atom_header_size = sizeof(ngx_mp4_atom_header64_t);

                if (atom_size < sizeof(ngx_mp4_atom_header64_t)) {
                    ngx_log_error(NGX_LOG_ERR, mp4->file.log, 0,
                                 "\"%s\" mp4 atom is too small:%uL",
                                 mp4->file.name.data, atom_size);
                    return NGX_ERROR;
                }
            } else {
                ngx_log_error(NGX_LOG_ERR, mp4->file.log, 0,
                             "\"%s\" mp4 atom is too small:%uL",
                             mp4->file.name.data, atom_size);
                return NGX_ERROR;
            }
        }
```

size 字段可能有多个取值:

* 为0表示这是最后一个 atom
* 为1表示 `atom_size` 是64位的，其真实数值在`name`字段之后

由于`atom_size`为 header 和 body 的总大小，所以`atom_size`至少要大于等于8(header32的大小)才是正常，所以使用0和1表示特殊的 size 不会引起冲突。

```c
    if (ngx_http_mp4_read(mp4, sizeof(ngx_mp4_atom_header_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    atom_header = mp4->buffer_pos;
    atom_name = atom_header + sizeof(uint32_t);

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, mp4->file.log, 0
                   "mp4 atom: %*s @%0:%uL",
                   (size_t) 4, atom_name, mp4->offset, atom_size);

    if (atom_size > (uint64_t) (NGX_MAX_OFF_T_VALUE - mp4->offset)
        || mp4->offset + (off_t) atom_size > end)
    {
        ngx_log_error(NGX_LOG_ERR, mp4->file.log, 0,
                     "\"%s\" mp4 atom too large:%uL",
                     mp4->file.name.data, atom_size);
        return NGX_ERROR;
    }
```

上面再次读取`sizeof(ngx_mp4_atom_header_t)`大小的数据，这里其实我是有一点疑问的，前面不是已经读过了么，为什么还要读呢？
其实仔细看前面，函数一开始只是读取了`size`字段，如果`atom_size < sizeof(ngx_mp4_atom_header_t)`这个条件并未满足，那么就不会进入`if`语句块读取(当`atom_size == 1`时会读取整个 header)。所以其实并没有读取完整个 header，而这里后面需要用到 header 中的`name`字段以确定使用哪个解析函数来解析 atom，所以需要读取整个 header(其实只是`size`和`name`两个字段)

```c
        for (n = 0; atom[n].name; n++) {
            if (ngx_strncmp(atom_name, atom[n].name, 4) == 0) {

                // 跳过 header
                ngx_mp4_atom_next(mp4, atom_header_size);

                // 解析 atom-body
                rc = atom[n].handler(mp4, atom_size - atom_header_size);
                if (rc != NGX_OK) {
                    return rc;
                }

                goto next;
            }

        }

        // 这里应该这个 atom 找不到对应的 handler
        // 所以就直接跳过，不处理
        mp4_atom_next(mp4, atom_size);

    next:
        // 读取并解析下一个 atom
        continue;
    }

    return NGX_OK;
}
```

在一个`for`循环中，根据 atom-header 中的`name`字段为正在读取的 atom 找到其对应的 handler(解析函数)，并使用该 handler 解析该 atom。如果没有找到对应的 handler，就跳过该 atom。然后继续读取并解析下一个 atom
