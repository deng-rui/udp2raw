"""Run with: python3 tests/test_tcp_transport.py /path/to/udp2raw [TestCase.test]."""

import base64
import os
from pathlib import Path
import select
import shlex
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
RUNNER = shlex.split(os.environ.get("UDP2RAW_TEST_RUNNER", ""))
KEY = "tcp-test-shared-key"


def authority(address):
    host, port = address[:2]
    return f"[{host}]:{port}" if ":" in host else f"{host}:{port}"


def unused_port(host="127.0.0.1", kind=socket.SOCK_STREAM):
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, kind) as sock:
        sock.bind((host, 0))
        return sock.getsockname()[1]


def receive_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data.extend(chunk)
    return bytes(data)


def receive_record(sock):
    length = struct.unpack("!H", receive_exact(sock, 2))[0]
    return receive_exact(sock, length)


def plain_record(kind, sender, receiver, seq, conv=0, data=b""):
    plain = os.urandom(16) + b"U2T2" + kind + sender + receiver + struct.pack("!QI", seq, conv) + data
    return struct.pack("!H", len(plain)) + plain


class Process:
    def __init__(self, path, args):
        self.path = path
        self.output = path.open("wb")
        self.process = subprocess.Popen(
            [*RUNNER, BINARY, *args, "--disable-color"], stdout=self.output, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )

    def log(self):
        return self.path.read_text(encoding="utf-8", errors="replace")

    def wait_log(self, expected, timeout=8, occurrences=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = self.log()
            if text.count(expected) >= occurrences:
                return
            if self.process.poll() is not None:
                raise AssertionError(f"process exited {self.process.returncode}:\n{text}")
            time.sleep(0.02)
        raise AssertionError(f"missing {expected!r}:\n{self.log()}")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.output.close()


class Echo:
    def __init__(self, host="127.0.0.1"):
        family = socket.AF_INET6 if ":" in host else socket.AF_INET
        self.sock = socket.socket(family, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        self.sock.bind((host, 0))
        self.address = self.sock.getsockname()[:2]
        self.sock.settimeout(0.1)
        self.stopped = threading.Event()
        self.received = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stopped.is_set():
            try:
                data, source = self.sock.recvfrom(65536)
                self.received.append((data, source))
                self.sock.sendto(data, source)
            except socket.timeout:
                continue
            except OSError:
                break

    def close(self):
        self.stopped.set()
        self.sock.close()
        self.thread.join(timeout=2)


class Proxy:
    def __init__(self, target, credentials=None, response=None, split=False, coalesce=False, host="127.0.0.1"):
        self.target, self.credentials = target, credentials
        self.response, self.split, self.coalesce = response, split, coalesce
        self.sock = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind((host, 0))
        self.sock.listen(16)
        self.sock.settimeout(0.1)
        self.address = self.sock.getsockname()[:2]
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.active, self.threads, self.requests, self.errors = [], [], [], []
        self.forwarded_clients = set()
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while not self.stopped.is_set():
            try:
                client, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.active.append(client)
            thread = threading.Thread(target=self.handle, args=(client,), daemon=True)
            self.threads.append(thread)
            thread.start()

    def handle(self, client):
        upstream = None
        try:
            client.settimeout(3)
            header = bytearray()
            while not header.endswith(b"\r\n\r\n") and len(header) < 16384:
                header.extend(receive_exact(client, 1))
            header = bytes(header)
            self.requests.append(header)
            expected = f"CONNECT {authority(self.target)} HTTP/1.1\r\n".encode()
            if not header.startswith(expected) or f"Host: {authority(self.target)}\r\n".encode() not in header:
                self.errors.append("wrong CONNECT authority or Host")
                return
            if self.credentials:
                auth = b"Proxy-Authorization: Basic " + base64.b64encode(self.credentials.encode()) + b"\r\n"
                if auth not in header:
                    client.sendall(b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n")
                    return
            if self.response is not None:
                client.sendall(self.response)
                return
            upstream = socket.create_connection(self.target, timeout=3)
            with self.lock:
                self.active.append(upstream)
            pending = b""
            client.setblocking(False)
            try:
                while True:
                    chunk = client.recv(8192)
                    if not chunk:
                        return
                    pending += chunk
            except BlockingIOError:
                pass
            finally:
                client.settimeout(3)
            if pending:
                upstream.sendall(pending)
            response = b"HTTP/1.1 200 Connection Established\r\nProxy-Agent: loopback-test\r\n\r\n"
            if self.coalesce:
                initial = receive_record(upstream)
                response += struct.pack("!H", len(initial)) + initial
            if self.split:
                for byte in response[:12]:
                    client.sendall(bytes([byte]))
                    time.sleep(0.001)
                client.sendall(response[12:])
            else:
                client.sendall(response)
            while not self.stopped.is_set():
                readable, _, _ = select.select([client, upstream], [], [], 0.1)
                for source in readable:
                    data = source.recv(8192)
                    if not data:
                        return
                    destination = upstream if source is client else client
                    if source is client:
                        with self.lock:
                            self.forwarded_clients.add(id(client))
                    if self.split:
                        destination.sendall(data[:1])
                        destination.sendall(data[1:3])
                        destination.sendall(data[3:])
                    else:
                        destination.sendall(data)
        except (OSError, EOFError, ValueError):
            pass
        finally:
            for sock in (client, upstream):
                if sock is not None:
                    with self.lock:
                        if sock in self.active:
                            self.active.remove(sock)
                    sock.close()

    def disconnect(self):
        with self.lock:
            for sock in self.active:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def close(self):
        self.stopped.set()
        self.sock.close()
        self.disconnect()
        self.thread.join(timeout=2)
        for thread in self.threads:
            thread.join(timeout=2)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="udp2raw-tcp-test-")
        self.addCleanup(self.tmp.cleanup)
        self.processes = []

    def start(self, args):
        process = Process(Path(self.tmp.name) / f"process-{len(self.processes)}.log", args)
        self.processes.append(process)
        self.addCleanup(process.close)
        return process

    def server(self, host="127.0.0.1", cipher="aes128cbc", auth="hmac_sha1"):
        echo = Echo(host)
        self.addCleanup(echo.close)
        target = (host, unused_port(host))
        process = self.start(["-s", "-l", authority(target), "-r", authority(echo.address),
                              "--raw-mode", "tcp", "-k", KEY, "--cipher-mode", cipher, "--auth-mode", auth])
        process.wait_log("tcp mode TCP listening")
        return target, echo, process

    def client(self, target, proxy=None, credentials=None, host="127.0.0.1", key=KEY, cipher="aes128cbc", auth="hmac_sha1", ready=True, tcp_connections=1):
        local = (host, unused_port(host, socket.SOCK_DGRAM))
        args = ["-c", "-l", authority(local), "-r", authority(target), "--raw-mode", "tcp", "-k", key,
                "--cipher-mode", cipher, "--auth-mode", auth]
        if proxy:
            args += ["--http-proxy", "http://" + authority(proxy.address)]
        if credentials:
            args += ["--http-proxy-auth", credentials]
        if tcp_connections != 1:
            args += ["--tcp-connections", str(tcp_connections)]
        process = self.start(args)
        process.wait_log("tcp mode UDP listening")
        if ready:
            try:
                process.wait_log("tcp tunnel ready", occurrences=tcp_connections)
            except AssertionError as error:
                logs = "\n\n".join(item.log() for item in self.processes)
                raise AssertionError(f"{error}\nall process logs:\n{logs}")
        return local, process

    def proxy(self, target, **kwargs):
        proxy = Proxy(target, **kwargs)
        self.addCleanup(proxy.close)
        return proxy

    def udp_socket(self, host="127.0.0.1"):
        sock = socket.socket(socket.AF_INET6 if ":" in host else socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(4)
        sock.bind((host, 0))
        self.addCleanup(sock.close)
        return sock

    def exchange(self, sock, target, data):
        sock.sendto(data, target)
        self.assertEqual(data, sock.recvfrom(65536)[0])

    def test_authenticated_proxy_full_datagrams_and_multiplexing(self):
        target, echo, server = self.server()
        proxy = self.proxy(target, credentials="proxy-user:password:with:colons", split=True, coalesce=True)
        local, client = self.client(target, proxy, "proxy-user:password:with:colons")
        one, two = self.udp_socket(), self.udp_socket()
        for size in (0, 1, 1400, 1667, 1668, 1800, 32000, 65507):
            with self.subTest(size=size):
                self.exchange(one, local, os.urandom(size))
        one.sendto(b"first-peer", local)
        two.sendto(b"second-peer", local)
        self.assertEqual(b"first-peer", one.recvfrom(65536)[0])
        self.assertEqual(b"second-peer", two.recvfrom(65536)[0])
        addresses = {payload: source for payload, source in echo.received if payload in (b"first-peer", b"second-peer")}
        self.assertNotEqual(addresses[b"first-peer"], addresses[b"second-peer"])
        self.assertEqual([], proxy.errors)
        self.assertNotIn("password:with:colons", client.log())
        self.assertNotIn(KEY, client.log())
        self.assertNotIn("wpcap.dll", client.log())
        self.assertNotIn("wpcap.dll", server.log())

    def test_proxy_disconnect_reconnects(self):
        target, echo, _ = self.server()
        proxy = self.proxy(target)
        local, client = self.client(target, proxy)
        sock = self.udp_socket()
        self.exchange(sock, local, b"before disconnect")
        source_port_before = next(source[1] for payload, source in reversed(echo.received) if payload == b"before disconnect")
        proxy.disconnect()
        client.wait_log("tcp tunnel ready", occurrences=2)
        self.exchange(sock, local, b"after reconnect")
        source_port_after = next(source[1] for payload, source in reversed(echo.received) if payload == b"after reconnect")
        self.assertEqual(source_port_before, source_port_after)
        self.assertGreaterEqual(len(proxy.requests), 2)

    def test_independent_clients_and_cipher_modes(self):
        for cipher, auth in (("aes128cbc", "md5"), ("aes128cfb", "hmac_sha1"), ("xor", "simple"), ("none", "hmac_sha1")):
            with self.subTest(cipher=cipher, auth=auth):
                target, _, _ = self.server(cipher=cipher, auth=auth)
                one, _ = self.client(target, cipher=cipher, auth=auth)
                two, _ = self.client(target, cipher=cipher, auth=auth)
                self.exchange(self.udp_socket(), one, os.urandom(1800))
                self.exchange(self.udp_socket(), two, os.urandom(32000))

    def test_parallel_tcp_mux_connections(self):
        target, echo, _ = self.server()
        proxy = self.proxy(target, split=True, coalesce=True)
        local, client = self.client(target, proxy, tcp_connections=3)
        sockets = [self.udp_socket() for _ in range(6)]
        for index, sock in enumerate(sockets):
            self.exchange(sock, local, f"mux-peer-{index}".encode())
        self.assertGreaterEqual(len(proxy.requests), 3)
        self.assertGreaterEqual(len(proxy.forwarded_clients), 3)
        self.assertEqual({f"mux-peer-{index}".encode() for index in range(6)},
                         {payload for payload, _ in echo.received if payload.startswith(b"mux-peer-")})
        self.assertIn("tcp_connections=3", client.log())

    def test_ipv6_proxy_and_endpoints(self):
        try:
            unused_port("::1")
        except OSError:
            self.skipTest("IPv6 loopback is unavailable")
        target, _, _ = self.server(host="::1")
        proxy = self.proxy(target, host="::1", coalesce=True)
        local, _ = self.client(target, proxy, host="::1")
        self.exchange(self.udp_socket("::1"), local, os.urandom(1800))
        self.assertEqual([], proxy.errors)

    def test_proxy_denials_and_bad_responses_never_deliver_udp(self):
        target, echo, _ = self.server()
        cases = [
            (b"HTTP/1.1 407 Proxy Authentication Required\r\n\r\n", "HTTP 407"),
            (b"HTTP/1.1 403 Forbidden\r\n\r\n", "HTTP 403"),
            (b"HTTP/1.1 20X Invalid\r\n\r\n", "invalid HTTP proxy"),
            (b"HTTP/1.1 200 OK\r\nX-Long: " + b"x" * 17000, "headers too large"),
        ]
        for response, expected in cases:
            with self.subTest(expected=expected):
                proxy = self.proxy(target, response=response)
                local, client = self.client(target, proxy, ready=False)
                self.udp_socket().sendto(b"must not arrive", local)
                client.wait_log(expected)
                self.assertNotIn("tcp tunnel ready", client.log())
                client.close()
        self.assertEqual([], echo.received)

    def test_wrong_tunnel_key_is_rejected(self):
        target, echo, _ = self.server()
        local, client = self.client(target, key="wrong-tunnel-key", ready=False)
        self.udp_socket().sendto(b"not authenticated", local)
        client.wait_log("tcp tunnel closed")
        self.assertNotIn("tcp tunnel ready", client.log())
        self.assertEqual([], echo.received)

    def open_plain(self, target):
        sock = socket.create_connection(target, timeout=2)
        self.addCleanup(sock.close)
        client_nonce = os.urandom(16)
        client_id = os.urandom(16)
        sock.sendall(plain_record(b"H", client_nonce, b"\x00" * 16, 1, data=client_id))
        challenge = receive_record(sock)
        self.assertEqual(b"U2T2C", challenge[16:21])
        self.assertEqual(client_nonce, challenge[37:53])
        return sock, client_nonce, challenge[21:37], client_id

    def assert_closed(self, sock, timeout=3):
        sock.settimeout(timeout)
        while True:
            try:
                if not sock.recv(4096):
                    return
            except ConnectionError:
                return

    def test_replayed_handshake_and_record_are_rejected(self):
        target, echo, _ = self.server(cipher="none", auth="none")
        sock, client_nonce, server_nonce, client_id = self.open_plain(target)
        hello = plain_record(b"H", client_nonce, b"\x00" * 16, 1, data=client_id)
        sock.sendall(plain_record(b"A", client_nonce, server_nonce, 2))
        data = plain_record(b"D", client_nonce, server_nonce, 3, 7, struct.pack("!HH", 4, 0) + b"once")
        sock.sendall(data)
        self.assertEqual(b"once", receive_record(sock)[69:])
        sock.sendall(data)
        self.assert_closed(sock)
        second = socket.create_connection(target, timeout=2)
        self.addCleanup(second.close)
        second.sendall(hello)
        self.assert_closed(second)
        self.assertEqual([b"once"], [data for data, _ in echo.received])

    def test_invalid_lengths_and_stalled_handshake_are_closed(self):
        target, _, _ = self.server(cipher="none", auth="none")
        for size in (0, 64, 1801, 65535):
            with self.subTest(size=size):
                sock, _, _, _ = self.open_plain(target)
                sock.sendall(struct.pack("!H", size))
                self.assert_closed(sock)
        partial, _, _, _ = self.open_plain(target)
        partial.sendall(b"\x00")
        self.assert_closed(partial, timeout=7)

    def test_invalid_options_fail_before_network_setup(self):
        base = [*RUNNER, BINARY, "-c", "-l", "127.0.0.1:45671", "-r", "127.0.0.1:45672", "--disable-color"]
        cases = [
            ["--http-proxy", "127.0.0.1:8080"],
            ["--raw-mode", "tcp", "--http-proxy-auth", "user:secret"],
            ["--raw-mode", "tcp", "--http-proxy", "https://127.0.0.1:8080"],
            ["--raw-mode", "tcp", "--http-proxy", "user:secret@localhost:8080"],
            ["--raw-mode", "tcp", "--http-proxy", "localhost:65536"],
            ["--raw-mode", "tcp", "--http-proxy", "localhost:0"],
            ["--raw-mode", "tcp", "--http-proxy", "localhost:80/path"],
            ["--raw-mode", "tcp", "--http-proxy", "localhost:80", "--http-proxy-auth", "secret-no-colon"],
            ["--raw-mode", "tcp", "--http-proxy-auth=user:secret"],
            ["--raw-mode", "tcp", "--http-proxy=user:secret@localhost:80"],
            ["--raw-mode", "tcp", "--tcp-connections", "0"],
            ["--raw-mode", "tcp", "--tcp-connections", "17"],
            ["--raw-mode", "tcp", "--key=secret"],
            ["--raw-mode", "tcp", "-ksecret", "-kduplicate"],
            ["--raw-mode", "tcp", "--easy-tcp"],
            ["--raw-mode", "tcp", "-g"],
        ]
        for args in cases:
            with self.subTest(args=args):
                result = subprocess.run(base + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5)
                self.assertNotEqual(0, result.returncode, result.stdout.decode(errors="replace"))
                self.assertNotIn(b"secret", result.stdout)
                self.assertNotIn(b"listening", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
