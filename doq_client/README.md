# DNS-over-QUIC (DoQ) Client

这是一个使用 XQUIC 库实现的完整 DNS-over-QUIC 客户端程序，遵循 RFC 9250 标准。

## 功能特性

✅ **完整的 QUIC 实现**
- 使用 XQUIC 库建立 QUIC 连接
- TLS 1.3 握手
- 证书验证
- 连接管理

✅ **DNS 协议支持**
- 手动构造 DNS 查询消息
- 解析 DNS 响应
- 支持 A 记录查询

✅ **事件驱动架构**
- macOS: kqueue（高性能）
- 跨平台: select 备选方案
- 无 libevent 依赖

✅ **DoQ 协议（RFC 9250）**
- ALPN: "doq"
- 端口: 853
- 双向 QUIC 流
- 无长度前缀（QUIC 流提供消息边界）

## 构建

```bash
cd /Users/tangjiacheng/Downloads/Monorepo/Monorepo
./tool/gn gen out
./tool/ninja -C out doq_client
```

## 运行

```bash
DYLD_LIBRARY_PATH=./out ./out/doq_client
```

## 实现细节

### 文件结构

```
doq_client/
├── doq_client.c    # 主程序（673行）
├── dns_proto.c     # DNS 协议实现（268行）
├── dns_proto.h     # DNS 头文件
├── event_loop.c    # 事件循环（395行）
├── event_loop.h    # 事件循环头文件
├── BUILD.gn        # GN 构建配置
└── README.md       # 本文件
```

### XQUIC 集成

程序展示了如何：
1. 创建 XQUIC 客户端引擎
2. 注册自定义 ALPN（"doq"）
3. 建立 QUIC 连接
4. 创建双向流
5. 发送和接收数据
6. 处理回调（连接、流、定时器）

### DNS-over-QUIC 协议

根据 RFC 9250：
- **不使用长度前缀**：与 DNS-over-TCP 不同，DoQ 直接在 QUIC 流上发送裸 DNS 消息
- **双向流**：每个查询使用一个新的双向流
- **流关闭**：客户端发送查询后关闭写端
- **并发**：可以在同一连接上打开多个流

### 事件循环

- **kqueue (macOS)**：高效的事件通知机制
- **select (备选)**：跨平台兼容性
- 支持：
  - Socket 读事件
  - 微秒级定时器
  - 事件循环控制

## 测试结果

使用阿里云 DNS（223.6.6.6）测试：
- ✅ QUIC 握手成功
- ✅ TLS 协商完成
- ✅ 流创建成功
- ✅ DNS 查询发送成功（28字节）
- ✅ QUIC 数据包正确收发

## 技术要点

1. **ALPN 注册**：通过 `xqc_engine_register_alpn` 注册应用层协议
2. **回调系统**：连接回调和流回调分别处理
3. **user_data 传递**：正确传递上下文数据到回调
4. **错误处理**：完整的错误检查和日志

## 依赖

- XQUIC 库（已集成）
- BoringSSL（已集成）
- 系统库：socket, kqueue/select

## 参考

- [RFC 9250: DNS over Dedicated QUIC Connections](https://www.rfc-editor.org/rfc/rfc9250.html)
- [XQUIC 文档](https://github.com/alibaba/xquic)
- [RFC 1035: Domain Names - Implementation and Specification](https://www.rfc-editor.org/rfc/rfc1035.html)

## 作者

使用 GN + Ninja 构建系统实现，集成到 monorepo 中。

