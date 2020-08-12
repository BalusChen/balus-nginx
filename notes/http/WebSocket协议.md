# WebSocket 协议

>   The WebSocket Protocol is an independent TCP-based protocol.  Its
    only relationship to HTTP is that its handshake is interpreted by
    HTTP servers as an Upgrade request.

## 握手阶段

```http request
        GET /chat HTTP/1.1
        Host: server.example.com
        Upgrade: websocket
        Connection: Upgrade
        Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
        Origin: http://example.com
        Sec-WebSocket-Protocol: chat, superchat
        Sec-WebSocket-Version: 13
```

```http response
        HTTP/1.1 101 Switching Protocols
        Upgrade: websocket
        Connection: Upgrade
        Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
        Sec-WebSocket-Protocol: chat
```

## 分片

      0                   1                   2                   3
      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     +-+-+-+-+-------+-+-------------+-------------------------------+
     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
     | |1|2|3|       |K|             |                               |
     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
     |     Extended payload length continued, if payload len == 127  |
     + - - - - - - - - - - - - - - - +-------------------------------+
     |                               |Masking-key, if MASK set to 1  |
     +-------------------------------+-------------------------------+
     | Masking-key (continued)       |          Payload Data         |
     +-------------------------------- - - - - - - - - - - - - - - - +
     :                     Payload Data continued ...                :
     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
     |                     Payload Data continued ...                |
     +---------------------------------------------------------------+

1. fin：表示这是 message 中的最后一个 frame（第一个也可能是最后一个）
2. rsv[1-3]：除非双方协商好了非零值的含义，否则这三个 bit 都应该为 0
3. opcode：定义了 payload 的解释方法
4. mask：payload 是否被掩码
5. 

### opcode

opcode 为 0，表示

> Control frames MAY be injected in the middle of a fragmented message.  Control frames themselves MUST NOT be fragmented.

>  Control frames are used to communicate state about the WebSocket. All control frames MUST have a payload length of 125 bytes or less and MUST NOT be fragmented.

> As a consequence of these rules, all fragments of a message are of the same type, as set by the first fragment's opcode.  Since control frames cannot be fragmented, the type for all fragments in a message MUST be either text, binary, or one of the reserved opcodes.

> NOTE: If control frames could not be interjected, the latency of a ping, for example, would be very long if behind a large message. Hence, the requirement of handling control frames in the middle of a fragmented message.

总结：

1. control message 不能被 fragment，也就是说一个 control frame 就是一个 control message，但是需要注意，control frame 可能夹杂在正常的 message 分片中，而 endpoint 需要处理这种情况。
2. 如果 control frame 不能插入的话，比如 ping frame，在一个很大的 message 之后的话，就得等好长时间才能得到响应
3. control frame 的 payload 的长度必须 <= 125 字节
2. message 第一个 frame 决定该 frame 的类型（即 opcode），后续的 frame 的 opcode 为 0，

frame 和 fragment 的区别？ message fragment 一下就成 frame 了

### 掩码

1. To avoid confusing network intermediaries (such as intercepting proxies) and for security reasons, a client MUST mask all frames that it sends to the server (Note that masking is done whether or not the WebSocket Protocol is running over TLS.).
2. The server MUST close the connection upon receiving a frame that is not masked.  In this case, a server MAY send a Close frame with a status code of 1002 (protocol error)
3. A server MUST NOT mask any frames that it sends to the client.  A client MUST close a connection if it detects a masked frame.  In this case, it MAY use the status code 1002 (protocol error) 

总结：

1. client 到 server 的 frame 必须使用掩码，如果 server 接收到没有掩码的 frame，以错误码为 1002 的 close frame 响应 client
2. server 到 client 的 frame 不能使用掩码，如果 client 接收到带有掩码的 frame，以错误码为 1002 的 close frame 响应 server

### 控制帧

Control Frame，通过最高位为1的 opcode 来标识，它们是用来交流彼此的状态的，控制帧经常会插到

> Control frames are used to communicate state about the WebSocket. Control frames can be interjected in the middle of a fragmented message.

> All control frames MUST have a payload length of 125 bytes or less and MUST NOT be fragmented.


### 关闭帧

Close Frame，是指 opcode=0x8 的帧，它可能包含 payload，用以描述关闭原因（比如对端关闭，对端接收的包太大，或者格式不符合预期），如果它包含 body 的话，那么 body 的前两个字节一定是无符号整数（网络序），用以表示状态码（close code）。

* 如果 close frame 是从 client 发送至 server 的，那么这个 frame 必须被 mask
* 如果一段接收到了 close frame，但是并没有发送过 close frame，那么它必须发送一个 close frame 作为相应，一般是回显其接收到的 status code
* 当发送并接收（或者反过来）到了 close frame 之后，那么 WebSocket 连接就可以被认为是关闭了，那么就必须关闭 TCP 连接。服务端必须尽快关闭，当然 client 也可以随时关闭（比如一段时间都 server 都没有关闭 TCP 连接）

当发送或者接收到 close frame，就表示 WebSocket 挥手阶段已经开始了，WebSocket 连接处于 CLOSED 状态了，当 TCP 连接完全关闭时，就说 WebSocket 连接已经被干净地（cleanly）关闭了。

如果 WebSocket 连接无法建立，我们也说这个 WebSocket 连接是 CLOSED，但是并不干净（cleanly）

#### Close Code

#### Close Reason

### Ping 帧

Ping Frame，当接收到了 ping frame 之后，本端必须尽早发送 pong frame（除非已经接收到了 close frame），ping frame 也可以携带数据，如果携带了数据的话，pong frame 必须将这些数据回显至对端

ping frame 有两个作用：

* keepalive
* 检查对端是否还可以响应

### 数据帧

Data Frame，通过最高位为 0 的 opcode 来标识：

* Text：Payload Data 通过
* Binary：

## 关闭连接

> The underlying TCP connection, in most normal cases, SHOULD be closed first by the server, so that it holds the TIME_WAIT state and not the client (as this would prevent it from re-opening the connection for 2 maximum segment lifetimes (2MSL), while there is no corresponding server impact as a TIME_WAIT connection is immediately reopened upon a new SYN with a higher seq number)

## 参考

[RFC6455: WebSocket](https://tools.ietf.org/html/rfc6455)

[WebSocket协议以及ws源码分析](https://juejin.im/post/5ce8976151882533441ecc20)
