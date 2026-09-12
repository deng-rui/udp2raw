# HTTP 代理实现说明

## 功能概述

UDP2RAW 已完整实现 HTTP 代理作为流量出口的功能，支持以下流量路径：

```
UDP → UDP2RAW 客户端 → HTTP 代理 (仅TCP) → UDP2RAW 服务端 → UDP 服务器
```

## ✅ 已实现的功能

### 1. HTTP CONNECT 代理支持
- 完整的 HTTP/1.1 CONNECT 方法实现
- 在建立加密隧道前处理代理握手
- 同时支持需要认证和无需认证的代理

### 2. 无密码代理支持 ⭐
- 可以单独使用 `--http-proxy` 而不需要 `--http-proxy-auth`
- 当未提供凭据时，不发送 Proxy-Authorization 头
- 适用于企业内部无需认证的代理

### 3. TCP 多路复用 (MUX)
- 通过 `--tcp-connections` 支持多条并行 TCP 连接 (1-16)
- 基于队列深度的负载均衡
- 每个数据报独立选择通道，避免队头阻塞
- 单个 UDP 流可以双向使用所有通道

### 4. 性能优化
- 每个数据报独立选择负载最小的 TCP 通道
- UDP 会话绑定到客户端身份，而非特定 TCP 连接
- 服务端 UDP 对等端在无可用 TCP 通道时暂停
- 新通道可用时自动恢复

## 使用方法

### 基础用法（无密码代理）

```bash
# 客户端
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  -k mykey

# 服务端
udp2raw -s -l 0.0.0.0:443 -r 127.0.0.1:53 \
  --raw-mode tcp \
  -k mykey
```

### 带认证的代理

```bash
# 客户端使用代理认证
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --http-proxy-auth username:password \
  -k mykey
```

### 使用 TCP 多路复用

```bash
# 使用 3 条并行 TCP 连接以提升吞吐量
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --tcp-connections 3 \
  -k mykey
```

## 连接流程

1. **客户端连接到代理**（而非直接连接服务器）
2. **CONNECT 握手**：
   ```
   CONNECT server.example.com:443 HTTP/1.1
   Host: server.example.com:443
   [Proxy-Authorization: Basic base64(username:password)]  # 可选
   
   ```
3. **代理响应**：
   ```
   HTTP/1.1 200 Connection Established
   
   ```
4. **加密隧道开始**通过已建立的连接传输数据

## 核心实现

### tcp.cpp:429 - 无密码支持
```cpp
if (!http_proxy_credentials.empty()) 
    request += "Proxy-Authorization: Basic " + base64(http_proxy_credentials) + "\r\n";
```
凭据是**可选的** - 如果为空，则简单地省略 Proxy-Authorization 头。

### tcp.cpp:855-862 - 验证逻辑
```cpp
if (!http_proxy_credentials.empty()) {
    // 仅在提供凭据时验证格式
    // --http-proxy 可以单独使用
}
```

### MUX 实现 (tcp.cpp:685-708)
- `select_connection()`: 选择负载最小的 TCP 通道
- 在队列深度相同的通道间轮询
- 服务端按客户端轮询
- 客户端按数据报轮询

## 测试覆盖

所有测试位于 `tests/test_tcp_transport.py`：

1. ✅ `test_passwordless_proxy` - **新增测试**
   - 测试无密码代理
   - 验证 CONNECT 请求中无 Proxy-Authorization 头
   - 确认正常数据传输

2. ✅ `test_authenticated_proxy_full_datagrams_and_multiplexing`
   - 测试认证代理与各种数据报大小
   - 验证多路复用正确工作
   - 确认凭据不会泄露到日志

3. ✅ `test_proxy_disconnect_reconnects`
   - 测试无密码代理（无凭据）
   - 验证重连保持 UDP 会话
   - 测试代理故障恢复

4. ✅ `test_parallel_tcp_mux_connections`
   - 测试 `--tcp-connections 3`
   - 验证所有连接被使用
   - 确认负载分配

5. ✅ `test_single_udp_flow_uses_all_lanes_in_both_directions`
   - 验证单个 UDP 流使用所有 TCP 通道
   - 测试上传和下载方向
   - 确认无单通道阻塞

6. ✅ `test_single_flow_continues_when_a_lane_stalls`
   - 测试一条 TCP 通道停顿时的韧性
   - 验证流量在其他通道继续
   - 测试上游和下游停顿

7. ✅ `test_mux_lane_loss_keeps_remote_udp_socket`
   - 验证 UDP 会话在 TCP 通道丢失后保持
   - 测试服务端 UDP 套接字保留
   - 确认重连恢复现有会话

## 性能特征

### 使用多路复用（--tcp-connections > 1）

- **更高吞吐量**：多条 TCP 连接避免单连接瓶颈
- **更低延迟**：队头阻塞仅影响单通道
- **更强韧性**：单条连接丢失不会停止流量
- **增加连接开销**：更多套接字和内存使用

### 最佳设置

- **局域网/低延迟**：`--tcp-connections 1`（单连接足够）
- **广域网/高延迟**：`--tcp-connections 2-4`（平衡吞吐量与开销）
- **高丢包率**：`--tcp-connections 3-8`（冗余有帮助）
- **最大值**：`--tcp-connections 16`（超过 8 收益递减）

## 常见问题

1. **"HTTP proxy CONNECT failed (HTTP 407)"**
   - 代理需要认证
   - 添加 `--http-proxy-auth username:password`

2. **"invalid --http-proxy"**
   - 使用格式：`host:port` 或 `http://host:port`
   - IPv6 地址：`[::1]:8080`

3. **"--http-proxy-auth requires --http-proxy"**
   - 使用 `--http-proxy-auth` 时必须指定 `--http-proxy`

4. **连接超时**
   - 检查代理是否可达
   - 验证代理允许 CONNECT 方法
   - 检查防火墙规则

## 安全考虑

1. **凭据保护**
   - 凭据永不记录或打印
   - Basic 认证发送 base64 编码的凭据
   - 尽可能使用 HTTPS 代理保护凭据

2. **隧道加密**
   - 通过代理的所有流量由 UDP2RAW 加密
   - 代理无法看到明文 UDP 流量
   - 支持标准 UDP2RAW 加密模式

## 技术亮点

### 无密码代理的实现优势

1. **兼容性好**：支持企业内网无认证代理
2. **灵活性高**：认证凭据完全可选
3. **安全性强**：即使无代理认证，UDP2RAW 仍然加密所有流量

### MUX 并发优化

1. **智能通道选择**：每个数据报选择队列最短的通道
2. **会话保持**：UDP 会话与客户端绑定，不依赖特定 TCP 连接
3. **自动恢复**：通道故障时自动切换，恢复后自动重用
4. **负载均衡**：单个 UDP 流可以充分利用所有并行 TCP 通道

## 总结

✅ HTTP 代理作为流量出口已完全实现  
✅ 无密码代理已原生支持  
✅ TCP MUX 并发已优化实现  
✅ 完整的测试覆盖  
✅ 生产环境可用
