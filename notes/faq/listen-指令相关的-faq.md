# listen 指令相关的 faq

listen 指令支持许多形式的监听地址，还支持许多的参数，非常的复杂。

## default_server

`listen`指令有一个`default_server`参数：

```nginx
server {
    listen              80 default_server;
    server_name         www.example.com;
}
```

这个参数在 0.8.21 开始引入，在此之前应该使用`default`，它用于在


### Question

但是问题是，

* 这个参数是 port 维度的，还是 ip:port 二元组维度的？
* 什么样的请求会被`default_server`所处理？
* 没有设置这个参数的话，默认哪个 server是"默认"的呢？

### Answer

* 这个参数是 port 端口维度的

比如下面这份配置：

```nginx
server {
    listen          8888 default_server;
    server_name     www.example.com;
}

server {
    listen          8888 default_server;
    server_name     www.example.org;
}
```

两个`listen`指令都是监听的8888端口，虽然没有写ip地址，其实默认是0.0.0.0，所以其实都监听的是`0.0.0.0:8888`地址，如果用这份配置启动 nginx 的话，会报错：

```sh
nginx: [emerg] a duplicate default server for 0.0.0.0:8888 in ./conf/nginx.conf:75
```

再比如下面这份配置：

```nginx
server {
    listen          127.0.0.1:8888 default_server;
    server_name     www.example.com;
}

server {
    listen          192.168.1.107:8888 default_server;
    server_name     www.example.net;
}

server {
    listen          8888 default_server;
    server_name     www.example.org;
}
```

是可以成功启动的。
