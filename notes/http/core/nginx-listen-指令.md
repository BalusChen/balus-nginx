# nginx listen 指令

这两天在看 nginx 对监听端口的管理，发现这部分的处理逻辑很长，而且`listen`指令的用法很多样，导致我对这一块的代码看了很久也没有看懂，所以希望从源头开始看懂整个链路；那么首先就得看`listen`指令。

## 参考

[nginx listen directive manual](http://nginx.org/en/docs/http/ngx_http_core_module.html#listen)

[how nginx processes a request](http://nginx.org/en/docs/http/request_processing.html)

[understanding nginx server and location block selection algorithms](https://www.digitalocean.com/community/tutorials/understanding-nginx-server-and-location-block-selection-algorithms)
