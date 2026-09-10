"""Run as root in a private network namespace with loopback enabled.

CI: sudo unshare --net -- bash -euc 'ip link set lo up; exec python3 tests/test_raw_transport.py BINARY'
"""

from contextlib import ExitStack
import os
from pathlib import Path
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        self.server.received.append(data)
        sock.sendto(data, self.client_address)


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class RawTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() != 0 or os.readlink("/proc/self/ns/net") == os.readlink("/proc/1/ns/net"):
            raise RuntimeError("Run in a private network namespace as root; this test adds iptables rules")
        if {name for _, name in socket.if_nameindex()} != {"lo"}:
            raise RuntimeError("The test namespace must contain only loopback")

    def run_case(self, threads, compact, cipher, auth, gro, maximum):
        with ExitStack() as cleanup:
            directory = Path(cleanup.enter_context(tempfile.TemporaryDirectory(prefix="udp2raw-raw-")))
            echo = cleanup.enter_context(socketserver.UDPServer(("127.0.0.1", 0), Echo))
            echo.received = []
            echo_thread = threading.Thread(target=echo.serve_forever, kwargs={"poll_interval": 0.05})
            echo_thread.start()
            cleanup.callback(echo_thread.join, 3)
            cleanup.callback(echo.shutdown)
            processes = []

            def logs():
                return "\n".join(path.read_text(errors="replace") for _, path in processes)

            def stop(process):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                        self.fail("shutdown hung:\n" + logs())
                self.assertEqual(0, process.returncode, logs())

            def start(arguments):
                path = directory / f"process-{len(processes)}.log"
                output = cleanup.enter_context(path.open("wb"))
                process = subprocess.Popen([BINARY, *arguments], stdout=output, stderr=subprocess.STDOUT)
                processes.append((process, path))
                cleanup.callback(stop, process)
                return path

            def wait_log(path, message):
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline:
                    if message in path.read_text(errors="replace"):
                        return
                    if any(process.poll() is not None for process, _ in processes):
                        self.fail("process exited before ready:\n" + logs())
                    time.sleep(0.02)
                self.fail(f"missing {message!r}:\n" + logs())

            server_port = free_port(socket.SOCK_STREAM)
            client_port = free_port(socket.SOCK_DGRAM)
            options = ["--raw-mode", "faketcp", "--threads", str(threads), "--mtu", "1280",
                       "--cipher-mode", cipher, "--auth-mode", auth, "--disable-color",
                       "-k", "raw-regression-key", "-a"]
            if compact:
                options += ["--compact-tcp"]
            if gro:
                options += ["--fix-gro"]

            # SOCK_DGRAM packet sockets strip the link header; inspect actual wire sizes.
            capture = cleanup.enter_context(socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(0x0800)))
            capture.bind(("lo", 0))
            capture.settimeout(0.05)
            stopped = threading.Event()
            packets = []

            def record():
                while not stopped.is_set():
                    try:
                        data = capture.recv(65535)
                    except socket.timeout:
                        continue
                    if len(data) < 40 or data[0] >> 4 != 4 or data[9] != socket.IPPROTO_TCP:
                        continue
                    ip_header = (data[0] & 15) * 4
                    ports = struct.unpack_from("!HH", data, ip_header)
                    if server_port in ports:
                        total = struct.unpack_from("!H", data, 2)[0]
                        tcp_header = (data[ip_header + 12] >> 4) * 4
                        syn = bool(data[ip_header + 13] & 2)
                        packets.append((total, tcp_header, syn))

            capture_thread = threading.Thread(target=record)
            capture_thread.start()
            cleanup.callback(capture_thread.join, 3)
            cleanup.callback(stopped.set)

            server_log = start(["-s", "-l", f"127.0.0.1:{server_port}",
                                "-r", f"127.0.0.1:{echo.server_address[1]}", *options])
            wait_log(server_log, "now listening at")
            client_log = start(["-c", "-l", f"127.0.0.1:{client_port}",
                                "-r", f"127.0.0.1:{server_port}", *options])
            wait_log(client_log, "to client_ready")
            wait_log(server_log, "to server_ready")
            wait_log(client_log, f"maximum UDP payload={maximum},")

            udp = cleanup.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
            udp.bind(("127.0.0.1", 0))
            udp.settimeout(3)
            destination = ("127.0.0.1", client_port)
            for length in (0, 1, 64, maximum):
                data = os.urandom(length)
                udp.sendto(data, destination)
                self.assertEqual(data, udp.recv(65535), logs())

            # Exercise several outstanding encryptions and slot reuse in both directions.
            for batch in range(12):
                expected = [struct.pack("!II", batch, i) + os.urandom(256 + i) for i in range(32)]
                for data in expected:
                    udp.sendto(data, destination)
                self.assertEqual(expected, [udp.recv(65535) for _ in expected], logs())

            oversize = os.urandom(maximum + 1)
            udp.sendto(oversize, destination)
            wait_log(client_log, "packet exceeds outer MTU")
            udp.settimeout(0.2)
            with self.assertRaises(socket.timeout):
                udp.recv(65535)
            self.assertNotIn(oversize, echo.received)
            udp.settimeout(3)
            udp.sendto(b"after oversize", destination)
            self.assertEqual(b"after oversize", udp.recv(65535), logs())

            stopped.set()
            capture_thread.join(timeout=3)
            self.assertFalse(capture_thread.is_alive())
            self.assertTrue(packets, "no raw TCP packets captured")
            self.assertTrue(any(syn for _, _, syn in packets), "no SYN captured")
            self.assertTrue(any(not syn for _, _, syn in packets), "no data captured")
            for total, header, syn in packets:
                self.assertLessEqual(total, 1280)
                self.assertEqual((28 if syn else 20) if compact else (40 if syn else 32), header)
            self.assertNotIn("asynchronous packet send failed", logs())
            self.assertNotIn("packet worker queue full", logs())

    def test_threads_mtu_compact_and_gro(self):
        cases = [
            (0, False, "aes128cbc", "md5", False, 1177),
            (1, True, "aes128cbc", "hmac_sha1", False, 1193),
            (4, True, "aes128cfb", "hmac_sha1", True, 1196),
            (4, False, "aes128cbc", "hmac_sha1", True, 1177),
        ]
        for case in cases:
            with self.subTest(threads=case[0], compact=case[1], cipher=case[2], auth=case[3], gro=case[4]):
                self.run_case(*case)


if __name__ == "__main__":
    unittest.main(verbosity=2)
