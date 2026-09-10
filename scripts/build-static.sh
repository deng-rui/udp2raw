#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

arch=${1:-arm64}
output=${2:-build}
case "$arch" in
    arm64) target=aarch64-linux-musl ;;
    amd64) target=x86_64-linux-musl ;;
    x86) target=x86-linux-musl ;;
    armv7) target=arm-linux-musleabihf ;;
    *) printf 'Unsupported architecture: %s\n' "$arch" >&2; exit 2 ;;
esac
zig=${ZIG:-zig}
mkdir -p "$output"
revision=$(git rev-parse HEAD)
printf 'const char *gitversion = "%s";\n' "$revision" > "$output/git_version.h"

# 静态 musl 同时封装 libc、C++ 运行库和线程库，部署不需要额外 .so 文件。
# 此项目部分 assert 表达式执行必要初始化，Zig 的 -O2 默认 NDEBUG 必须撤销。
sources=()
for source in ./*.cpp; do
    case "$source" in ./pcap_wrapper.cpp) continue ;; esac
    sources+=("$source")
done
"$zig" c++ -target "$target" -std=c++11 -O2 -UNDEBUG -static -pthread \
    -Wno-error=date-time -I"$output" -I. -isystem libev \
    "${sources[@]}" lib/md5.cpp lib/pbkdf2-sha1.cpp lib/pbkdf2-sha256.cpp \
    lib/aes_faster_c/aes.cpp lib/aes_faster_c/wrapper.cpp \
    -lrt -o "$output/udp2raw_$arch"
printf 'Built %s/udp2raw_%s (%s)\n' "$output" "$arch" "$target"
