# udp2raw build guide

the guide on how to build udp2raw

## GitHub Actions builds

The repository workflow `.github/workflows/build.yml` runs on pushes, pull requests and manual dispatches. It builds the following targets on Ubuntu runners; no local compiler is required:

| Artifact | Architecture | Toolchain |
| --- | --- | --- |
| `udp2raw-linux-amd64` | Linux x86_64 | Zig 0.14.1, musl |
| `udp2raw-linux-x86` | Linux x86 (32-bit) | Zig 0.14.1, musl |
| `udp2raw-linux-arm` | Linux ARMv7 | Zig 0.14.1, musl |
| `udp2raw-linux-armv8` | Linux ARM64 / ARMv8 | Zig 0.14.1, musl |
| `udp2raw-windows-x86` | Windows x86 (32-bit) | MinGW-w64 POSIX |
| `udp2raw-windows-x64` | Windows x64 | MinGW-w64 POSIX |

Download artifacts from the completed run. Linux artifacts contain a static executable, a `.tar.gz` preserving its executable permission, and SHA256 sums. Windows artifacts contain an `.exe` and SHA256 sums. The workflow uploads artifacts; it does not publish GitHub Releases. Linux ARM binaries are intended for rooted Android too; see the [Android guide](android_guide.md) for kernel and permission requirements. Windows still requires Npcap for raw modes, even though its C++ and thread runtimes are statically linked.

CI checks ELF/PE architecture and runtime linkage, runs `--help` natively or under QEMU for Linux targets, and runs software/accelerated AES and worker queue tests on Linux x86_64. The x86_64 binary also runs UDP round trips, ordering, MTU rejection and wire-header checks inside a private network namespace. The TCP transport test runs for each Linux binary (with QEMU where needed) and on both Windows runners. Windows raw networking and Android hardware are not exercised by CI.

## Optional local static build

With Zig 0.14.1 installed, the same Linux build can be run from Bash:

```bash
bash scripts/build-static.sh arm64 build/arm64
bash scripts/build-static.sh armv7 build/armv7
bash scripts/build-static.sh amd64 build/amd64
bash scripts/build-static.sh x86 build/x86
```

Set `ZIG=/path/to/zig` when Zig is not on `PATH`. The script defaults to ARM64 and outputs `build/udp2raw_arm64`. It deliberately keeps assertions enabled because existing initialization code has side effects inside assertions.

## Build udp2raw for a specific platform

### linux platform which supports local compile
such as PC,raspberry pi

##### install git
run on debian/ubuntun：
```
sudo apt-get install git
```
run on redhat/centos:
```
sudo yum install git
```
##### clone git code

run in any dir：

```
git clone https://github.com/wangyu-/udp2raw-tunnel.git
cd udp2raw-tunnel
```

##### install compile tool
run on debian/ubuntun：
```
sudo apt-get install build-essential
```

run on redhat/centos:
```
sudo yum groupinstall 'Development Tools'
```

run 'make'，compilation done. the udp2raw file is the just compiled binary

### platform which needs cross-compile
such as openwrt router,run following instructions on your PC

##### install git
run on debian/ubuntun：
```
sudo apt-get install git
```
run on redhat/centos:
```
sudo yum install git
```

##### download cross compile tool chain

find it on downloads.openwrt.org according to your openwrt version and cpu model.

for example, my tplink wdr4310 runs chaos_calmer 15.05,its with ar71xx cpu，download the following package.

```
http://downloads.openwrt.org/chaos_calmer/15.05/ar71xx/generic/OpenWrt-SDK-15.05-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64.tar.bz2
```
unzip it to any dir,such as ：/home/wangyu/OpenWrt-SDK-ar71xx-for-linux-x86_64-gcc-4.8-linaro_uClibc-0.9.33.2

cd into staging_dir ，toolchain-xxxxx ，bin .find the soft link with g++ suffix. in my case ,its mips-openwrt-linux-g++ ,check for its full path:

```
/home/wangyu/Desktop/OpenWrt-SDK-15.05-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64/staging_dir/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2/bin/mips-openwrt-linux-g++
```
##### compile
modify first line of makefile to:
```
cc_cross=/home/wangyu/Desktop/OpenWrt-SDK-15.05-ar71xx-generic_gcc-4.8-linaro_uClibc-0.9.33.2.Linux-x86_64/staging_dir/toolchain-mips_34kc_gcc-4.8-linaro_uClibc-0.9.33.2/bin/mips-openwrt-linux-g++
```

run `make cross`，the just generated `udp2raw_cross` is the binary,compile done. copy it to your router to run.

`make cross` generates non-static binary. If you have any problem on running it,try to compile a static binary by using `make cross2` or `make cross3`.If your toolchain supports static compiling, usually one of them will succeed. The generated file is still named `udp2raw_cross`.



## Build a full release (include all binaries supported in the makefile)

1. make sure your linux is amd64 version

2. clone the repo

3. make sure you have g++ , make sure your g++ support the `-m32` option; make your your have installed libraries for `-m32` option

4. download https://github.com/wangyu-/files/releases/download/files/toolchains.tar.gz , and extract it to the right position (according to the makefile)

5. run `make release` inside udp2raw's directory
