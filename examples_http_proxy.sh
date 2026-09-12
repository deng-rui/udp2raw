#!/bin/bash
# UDP2RAW HTTP 代理使用示例

# 场景 1: 无密码 HTTP 代理（企业内网代理）
# ===========================================

echo "示例 1: 使用无密码 HTTP 代理"
echo "客户端命令:"
cat <<'EOF'
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  -k mykey
EOF

echo ""
echo "服务端命令:"
cat <<'EOF'
udp2raw -s -l 0.0.0.0:443 -r 127.0.0.1:53 \
  --raw-mode tcp \
  -k mykey
EOF

echo ""
echo "流量路径: UDP应用 -> UDP2RAW客户端 -> HTTP代理 -> UDP2RAW服务端 -> UDP服务器"
echo ""

# 场景 2: 需要认证的 HTTP 代理
# =============================

echo "示例 2: 使用需要认证的 HTTP 代理"
echo "客户端命令:"
cat <<'EOF'
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --http-proxy-auth username:password \
  -k mykey
EOF

echo ""

# 场景 3: 使用 TCP 多路复用提升性能
# ==================================

echo "示例 3: 使用 3 条并行 TCP 连接（MUX）提升吞吐量"
echo "客户端命令:"
cat <<'EOF'
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --tcp-connections 3 \
  -k mykey
EOF

echo ""
echo "优势："
echo "  - 更高吞吐量：多条 TCP 连接避免单连接瓶颈"
echo "  - 更低延迟：队头阻塞仅影响单通道"
echo "  - 更强韧性：单条连接丢失不会停止流量"
echo ""

# 场景 4: 实际应用场景 - DNS over UDP2RAW
# ========================================

echo "示例 4: 通过 HTTP 代理加密传输 DNS 查询"
echo ""
echo "服务端（在公网服务器上运行）:"
cat <<'EOF'
udp2raw -s -l 0.0.0.0:443 -r 8.8.8.8:53 \
  --raw-mode tcp \
  -k your-secret-key
EOF

echo ""
echo "客户端（在防火墙内运行）:"
cat <<'EOF'
udp2raw -c -l 127.0.0.1:5353 -r your-server.com:443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --tcp-connections 2 \
  -k your-secret-key
EOF

echo ""
echo "配置 DNS 服务器为 127.0.0.1:5353"
echo ""

# 场景 5: 游戏 UDP 流量通过代理
# ==============================

echo "示例 5: 游戏 UDP 流量通过 HTTP 代理"
echo ""
echo "服务端（在公网服务器上运行）:"
cat <<'EOF'
udp2raw -s -l 0.0.0.0:8443 -r game-server.com:27015 \
  --raw-mode tcp \
  --cipher-mode aes128cbc \
  --auth-mode md5 \
  -k your-secret-key
EOF

echo ""
echo "客户端（在防火墙内运行）:"
cat <<'EOF'
udp2raw -c -l 127.0.0.1:27015 -r your-server.com:8443 \
  --raw-mode tcp \
  --http-proxy proxy.company.com:8080 \
  --tcp-connections 4 \
  --cipher-mode aes128cbc \
  --auth-mode md5 \
  -k your-secret-key
EOF

echo ""
echo "配置游戏服务器地址为 127.0.0.1:27015"
echo ""

# 性能调优建议
# ============

echo "性能调优建议："
echo ""
echo "局域网/低延迟环境："
echo "  --tcp-connections 1    # 单连接足够"
echo ""
echo "广域网/高延迟环境："
echo "  --tcp-connections 2-4  # 平衡吞吐量与开销"
echo ""
echo "高丢包率环境："
echo "  --tcp-connections 3-8  # 冗余有帮助"
echo ""
echo "注意: 超过 8 条连接收益递减，最大值为 16"
echo ""

# IPv6 代理支持
# =============

echo "IPv6 代理服务器示例："
cat <<'EOF'
udp2raw -c -l 127.0.0.1:4096 -r server.example.com:443 \
  --raw-mode tcp \
  --http-proxy [2001:db8::1]:8080 \
  -k mykey
EOF

echo ""
echo "注意: IPv6 地址必须用方括号包裹"
