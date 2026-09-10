# Run udp2raw on rooted Android

Use the `udp2raw-linux-armv8` artifact for ARM64 (`aarch64`), or
`udp2raw-linux-arm` for ARMv7 (`armv7l`), from a successful GitHub Actions run.
The executable statically links musl, C++ and pthread runtimes, so no extra
`.so` files or Termux runtime are required. There is no Android GUI or APK.

This guide uses a root shell, kernel raw-socket support and working `iptables`.
Root alone does not bypass every device's SELinux policy. Builds and emulated
startup are checked in CI; Android device networking is not tested by that
workflow.

## Install

Check the architecture with `uname -m`. Place the matching executable in an
executable directory such as `/data/local/tmp`, not shared storage (`/sdcard`
is commonly mounted with `noexec`). The following commands use ARM64:

```sh
su
cd /data/local/tmp
chmod 755 udp2raw-linux-armv8
./udp2raw-linux-armv8 --help
```

## Configure FakeTCP

Generate the rule first, using `-g` instead of automatic rule management:

```sh
./udp2raw-linux-armv8 -c -r 44.55.66.77:9966 -l 127.0.0.1:4000 \
  -k your-shared-password --auth-mode hmac_sha1 -g
```

In the root shell, add the exact rule printed by the command. For the IPv4 endpoint above:

```sh
iptables -I INPUT -s 44.55.66.77/32 -p tcp -m tcp --sport 9966 -j DROP
```

Start the tunnel without `-g`. The server must use the same key, cipher and authentication mode:

```sh
./udp2raw-linux-armv8 -c -r 44.55.66.77:9966 -l 127.0.0.1:4000 \
  -k your-shared-password --auth-mode hmac_sha1 \
  --threads 4 --mtu 1280 --compact-tcp
```

Use `--compact-tcp` on both endpoints for savings in both directions. This example allows 1193 bytes of UDP payload on IPv4; configure the upper application's packet size accordingly. Start with `--threads 1` or `2` on smaller devices and measure CPU, throughput and battery use. The default `--threads 0` is synchronous.

When stopping, remove the rule you added manually:

```sh
iptables -D INPUT -s 44.55.66.77/32 -p tcp -m tcp --sport 9966 -j DROP
```
