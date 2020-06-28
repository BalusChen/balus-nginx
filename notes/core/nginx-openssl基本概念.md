# nginx openssl 基本概念

## SKI

## SNI

SNI 意思是 Server Name Indication，用于告诉 Server，client 需要连接到哪个域名。

当前 Server 上配置多个域名已经是很常见的事情了（比如 Nginx），对于 HTTP，nginx 是通过请求的`Host`头来选择不同的虚拟主机；但是对于 HTTPS，它使用 SSL/TLS，需要 SSL 证书才能建立连接，后续才能发送请求头，所以 HTTPS 就不能使用`Host`头了，那么可以通过 SNI，把`Host`的内容放到 SNI 中，这样 nginx 根据 SNI 发送合适的证书给客户端。这样 HTTPS 就和 HTTP 统一了。

## 参考

[https 和 SNI](https://zzyongx.github.io/blogs/https-SNI.html)

[HTTPS 精读之 TLS 证书校验](https://zhuanlan.zhihu.com/p/30655259)
