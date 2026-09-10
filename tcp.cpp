#include "common.h"
#include "connection.h"
#include "encrypt.h"
#include "log.h"
#include "misc.h"
#include "tcp.h"

#include <climits>
#include <memory>
#if !defined(__MINGW32__)
#include <netdb.h>
#include <netinet/tcp.h>
#endif

std::string http_proxy_address;
std::string http_proxy_credentials;

namespace {
const int nonce_size = 16;
const int record_header_size = 65;
const int datagram_header_size = 4;
const int crypto_overhead_limit = 64;
const int chunk_limit = max_data_len - record_header_size - datagram_header_size - crypto_overhead_limit;
const int datagram_limit = 65507;
const size_t http_header_limit = 16384;
const size_t connection_queue_limit = 1024 * 1024;
const size_t total_queue_limit = 16 * connection_queue_limit;
const int io_batch_limit = 64;
const char protocol_magic[] = "U2T1";

void put_u64(char *data, u64_t value) {
    value = hton64(value);
    memcpy(data, &value, sizeof(value));
}

u64_t get_u64(const char *data) {
    u64_t value;
    memcpy(&value, data, sizeof(value));
    return ntoh64(value);
}

bool would_block(int error) {
#if defined(__MINGW32__)
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool interrupted(int error) {
#if defined(__MINGW32__)
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool connect_pending(int error) {
#if defined(__MINGW32__)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS || error == EINTR;
#endif
}

int socket_fd(my_fd_t fd) {
    if (fd == (my_fd_t)-1) return -1;
    if ((u64_t)fd > INT_MAX) {
        sock_close(fd);
        mylog(log_error, "socket handle exceeds libev fd range\n");
        return -1;
    }
    return (int)fd;
}

int new_socket(address_t address, int type) {
    int fd = socket_fd(socket(address.get_type(), type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP));
    if (fd < 0) return -1;
    setnonblocking(fd);
    set_buf_size(fd, socket_buf_size);
    return fd;
}

void random_nonce(char *data) {
    for (int i = 0; i < nonce_size; i += sizeof(u32_t)) {
        u32_t value = get_true_random_number_nz();
        memcpy(data + i, &value, sizeof(value));
    }
}

bool zero_nonce(const char *data) {
    const char zero[nonce_size] = {};
    return memcmp(data, zero, nonce_size) == 0;
}

bool proxy_endpoint(string &host, string &port) {
    string value = http_proxy_address;
    if (value.compare(0, 7, "http://") == 0) value.erase(0, 7);
    if (value.empty() || value.size() > 300) return false;
    for (unsigned char c : value)
        if (c <= 32 || c >= 127 || c == '@' || c == '/' || c == '?' || c == '#') return false;
    if (value[0] == '[') {
        size_t end = value.find(']');
        if (end == string::npos || end + 1 >= value.size() || value[end + 1] != ':') return false;
        host = value.substr(1, end - 1);
        in6_addr parsed;
        if (inet_pton(AF_INET6, host.c_str(), &parsed) != 1) return false;
        port = value.substr(end + 2);
    } else {
        size_t colon = value.find(':');
        if (colon == string::npos || value.find(':', colon + 1) != string::npos) return false;
        host = value.substr(0, colon);
        port = value.substr(colon + 1);
        for (unsigned char c : host)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    if (host.empty() || port.empty() || port.size() > 5) return false;
    unsigned number = 0;
    for (char c : port) {
        if (c < '0' || c > '9') return false;
        number = number * 10 + c - '0';
    }
    return number != 0 && number <= 65535;
}

string base64(const string &input) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string output;
    for (size_t i = 0; i < input.size(); i += 3) {
        unsigned value = (unsigned char)input[i] << 16;
        if (i + 1 < input.size()) value |= (unsigned char)input[i + 1] << 8;
        if (i + 2 < input.size()) value |= (unsigned char)input[i + 2];
        output += alphabet[(value >> 18) & 63];
        output += alphabet[(value >> 12) & 63];
        output += i + 1 < input.size() ? alphabet[(value >> 6) & 63] : '=';
        output += i + 2 < input.size() ? alphabet[value & 63] : '=';
    }
    return output;
}

struct tcp_context;
struct tcp_connection;

struct udp_peer {
    tcp_connection &connection;
    u32_t conv;
    int fd;
    ev_io reader;
    udp_peer(tcp_connection &connection, u32_t conv, int fd);
    ~udp_peer();
    static void read_cb(struct ev_loop *, ev_io *watcher, int);
};

enum class tcp_phase { connecting, proxy, challenge, hello, accept, ready, dead };

struct tcp_connection {
    tcp_context &context;
    int fd;
    tcp_phase phase;
    ev_io reader, writer;
    char local_nonce[nonce_size];
    char remote_nonce[nonce_size] = {};
    u64_t send_sequence = 1, recv_sequence = 1;
    u64_t started = get_current_time(), last_received = started, last_sent = started;
    string input, output;
    size_t written = 0, proxy_bytes = 0;
    unordered_map<u32_t, unique_ptr<udp_peer>> peers;
    lru_collector_t<u32_t> peer_lru;
    bool assembling = false;
    u32_t incoming_conv = 0;
    unsigned incoming_size = 0;
    u64_t assembly_started = 0;
    string datagram;

    tcp_connection(tcp_context &context, int fd, tcp_phase phase);
    ~tcp_connection();
    bool ready() const { return phase == tcp_phase::ready; }
    size_t queued() const { return output.size() - written; }
    void fail(const char *reason);
    void connected();
    void append_output(const string &bytes);
    bool encode(string &bytes, char type, u32_t conv, const char *data, int len);
    bool control(char type);
    void send_datagram(u32_t conv, const char *data, int len);
    void parse_input();
    bool process_record(char *plain, int len);
    bool receive_chunk(u32_t conv, char *data, int len);
    void deliver(u32_t conv, const char *data, int len);
    void tick(u64_t now);
    static void read_cb(struct ev_loop *, ev_io *watcher, int);
    static void write_cb(struct ev_loop *, ev_io *watcher, int);
};

struct tcp_context {
    struct ev_loop *loop;
    int local_fd = -1;
    ev_io local_reader;
    ev_timer timer;
    list<unique_ptr<tcp_connection>> connections;
    tcp_connection *client = NULL;
    conv_manager_t<address_t> client_peers;
    vector<address_t> destinations;
    size_t destination_index = 0;
    size_t udp_count = 0, queued_bytes = 0;
    u64_t next_connect = 0, queue_drops = 0;

    explicit tcp_context(struct ev_loop *loop) : loop(loop) {}
    void connect_client();
    void queue_drop() {
        ++queue_drops;
        if ((queue_drops & (queue_drops - 1)) == 0)
            mylog(log_warn, "tcp send queue full, dropped UDP datagrams=%llu\n", queue_drops);
    }
    static void local_cb(struct ev_loop *, ev_io *watcher, int);
    static void timer_cb(struct ev_loop *, ev_timer *watcher, int);
};

udp_peer::udp_peer(tcp_connection &connection, u32_t conv, int fd) : connection(connection), conv(conv), fd(fd) {
    ev_io_init(&reader, read_cb, fd, EV_READ);
    reader.data = this;
    ev_io_start(connection.context.loop, &reader);
    ++connection.context.udp_count;
}

udp_peer::~udp_peer() {
    ev_io_stop(connection.context.loop, &reader);
    sock_close(fd);
    --connection.context.udp_count;
}

void udp_peer::read_cb(struct ev_loop *, ev_io *watcher, int) {
    udp_peer &peer = *(udp_peer *)watcher->data;
    for (int i = 0; i < io_batch_limit && peer.connection.ready(); ++i) {
        char data[65536];
        int len = recv(peer.fd, data, sizeof(data), 0);
        if (len < 0) {
            int error = get_sock_errno();
            if (interrupted(error)) continue;
            if (!would_block(error)) mylog(log_warn, "tcp remote UDP receive failed: %s\n", get_sock_error());
            return;
        }
        peer.connection.peer_lru.update(peer.conv);
        peer.connection.send_datagram(peer.conv, data, len);
    }
}

tcp_connection::tcp_connection(tcp_context &context, int fd, tcp_phase phase) : context(context), fd(fd), phase(phase) {
    random_nonce(local_nonce);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ev_io_init(&reader, read_cb, fd, EV_READ);
    ev_io_init(&writer, write_cb, fd, EV_WRITE);
    reader.data = writer.data = this;
}

tcp_connection::~tcp_connection() {
    ev_io_stop(context.loop, &reader);
    ev_io_stop(context.loop, &writer);
    context.queued_bytes -= queued();
    if (fd >= 0) sock_close(fd);
}

void tcp_connection::fail(const char *reason) {
    if (phase == tcp_phase::dead) return;
    mylog(log_warn, "tcp tunnel closed: %s\n", reason);
    phase = tcp_phase::dead;
    ev_io_stop(context.loop, &reader);
    ev_io_stop(context.loop, &writer);
    for (auto &peer : peers) ev_io_stop(context.loop, &peer.second->reader);
    sock_close(fd);
    fd = -1;
}

void tcp_connection::append_output(const string &bytes) {
    if (written) {
        output.erase(0, written);
        written = 0;
    }
    output += bytes;
    context.queued_bytes += bytes.size();
    ev_io_start(context.loop, &writer);
}

bool tcp_connection::encode(string &bytes, char type, u32_t conv, const char *data, int len) {
    if (send_sequence == 0 || len > max_data_len - record_header_size - crypto_overhead_limit) return false;
    char plain[buf_len], encrypted[buf_len];
    // 固定 IV 的现有加密接口要求首块不可预测；会话双方的随机数另用于拒绝跨连接重放。
    random_nonce(plain);
    memcpy(plain + 16, protocol_magic, 4);
    plain[20] = type;
    memcpy(plain + 21, local_nonce, nonce_size);
    memcpy(plain + 37, remote_nonce, nonce_size);
    put_u64(plain + 53, send_sequence);
    write_u32(plain + 61, conv);
    if (len) memcpy(plain + record_header_size, data, len);
    int encrypted_len = record_header_size + len;
    if (my_encrypt(plain, encrypted, encrypted_len) != 0 || encrypted_len > max_data_len) return false;
    char length[2];
    write_u16(length, encrypted_len);
    bytes.append(length, sizeof(length));
    bytes.append(encrypted, encrypted_len);
    ++send_sequence;
    return true;
}

bool tcp_connection::control(char type) {
    if (queued() + max_data_len + 2 > connection_queue_limit || context.queued_bytes + max_data_len + 2 > total_queue_limit) {
        fail("control send queue limit");
        return false;
    }
    string bytes;
    if (!encode(bytes, type, 0, NULL, 0)) {
        fail("encryption failed");
        return false;
    }
    append_output(bytes);
    last_sent = get_current_time();
    return true;
}

void tcp_connection::send_datagram(u32_t conv, const char *data, int len) {
    if (!ready()) return;
    if (len < 0 || len > datagram_limit) {
        mylog(log_warn, "tcp mode UDP datagram exceeds %d bytes\n", datagram_limit);
        return;
    }
    int chunks = max(1, (len + chunk_limit - 1) / chunk_limit);
    size_t bound = len + chunks * (record_header_size + datagram_header_size + crypto_overhead_limit + 2);
    // 整个 UDP 报文一起入队，过载时整包丢弃，不能只发送一部分分片。
    if (queued() + bound > connection_queue_limit || context.queued_bytes + bound > total_queue_limit) {
        context.queue_drop();
        return;
    }
    string bytes;
    int offset = 0;
    do {
        int count = min(chunk_limit, len - offset);
        char chunk[datagram_header_size + chunk_limit];
        write_u16(chunk, len);
        write_u16(chunk + 2, offset);
        if (count) memcpy(chunk + datagram_header_size, data + offset, count);
        if (!encode(bytes, 'D', conv, chunk, count + datagram_header_size)) {
            fail("datagram encryption failed");
            return;
        }
        offset += count;
    } while (offset < len);
    append_output(bytes);
    last_sent = get_current_time();
}

void tcp_connection::connected() {
    phase = http_proxy_address.empty() ? tcp_phase::challenge : tcp_phase::proxy;
    ev_io_start(context.loop, &reader);
    if (phase == tcp_phase::proxy) {
        string authority = remote_addr.get_str();
        string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
        if (!http_proxy_credentials.empty()) request += "Proxy-Authorization: Basic " + base64(http_proxy_credentials) + "\r\n";
        append_output(request + "\r\n");
    }
}

void tcp_connection::write_cb(struct ev_loop *, ev_io *watcher, int) {
    tcp_connection &connection = *(tcp_connection *)watcher->data;
    if (connection.phase == tcp_phase::connecting) {
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(connection.fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) != 0 || error != 0) {
            connection.fail("TCP connect failed");
            return;
        }
        connection.connected();
    }
    for (int i = 0; i < io_batch_limit && connection.queued(); ++i) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int count = send(connection.fd, connection.output.data() + connection.written, (int)connection.queued(), flags);
        if (count < 0) {
            int error = get_sock_errno();
            if (interrupted(error)) continue;
            if (would_block(error)) return;
            connection.fail("TCP write failed");
            return;
        }
        if (count == 0) {
            connection.fail("TCP write returned zero");
            return;
        }
        connection.written += count;
        connection.context.queued_bytes -= count;
    }
    if (!connection.queued()) {
        string().swap(connection.output);
        connection.written = 0;
        ev_io_stop(connection.context.loop, &connection.writer);
    }
}

void tcp_connection::read_cb(struct ev_loop *, ev_io *watcher, int) {
    tcp_connection &connection = *(tcp_connection *)watcher->data;
    for (int i = 0; i < io_batch_limit; ++i) {
        char data[8192];
        int count = recv(connection.fd, data, sizeof(data), 0);
        if (count < 0) {
            int error = get_sock_errno();
            if (interrupted(error)) continue;
            if (!would_block(error)) connection.fail("TCP read failed");
            return;
        }
        if (count == 0) {
            connection.fail("peer disconnected");
            return;
        }
        connection.input.append(data, count);
        connection.parse_input();
        if (connection.phase == tcp_phase::dead) return;
    }
}

void tcp_connection::parse_input() {
    while (phase == tcp_phase::proxy) {
        size_t end = input.find("\r\n\r\n");
        if (end == string::npos) {
            if (proxy_bytes + input.size() > http_header_limit) fail("HTTP proxy response headers too large");
            return;
        }
        if (proxy_bytes + end + 4 > http_header_limit) {
            fail("HTTP proxy response headers too large");
            return;
        }
        size_t line_end = input.find("\r\n");
        if (line_end < 12 || (input.compare(0, 9, "HTTP/1.0 ") != 0 && input.compare(0, 9, "HTTP/1.1 ") != 0) ||
            (line_end > 12 && input[12] != ' ') || input[9] < '1' || input[9] > '5' ||
            input[10] < '0' || input[10] > '9' || input[11] < '0' || input[11] > '9') {
            fail("invalid HTTP proxy status line");
            return;
        }
        int status = (input[9] - '0') * 100 + (input[10] - '0') * 10 + input[11] - '0';
        proxy_bytes += end + 4;
        input.erase(0, end + 4);
        if (status >= 100 && status < 200 && status != 101) continue;
        if (status < 200 || status >= 300) {
            mylog(log_error, "HTTP proxy CONNECT failed (HTTP %d)\n", status);
            fail("HTTP CONNECT rejected");
            return;
        }
        phase = tcp_phase::challenge;
        // CONNECT 响应后同一次读取可能已经包含服务端挑战帧，必须保留这些字节。
    }
    size_t offset = 0;
    while (input.size() - offset >= 2) {
        int len = read_u16(&input[offset]);
        if (len < record_header_size || len > max_data_len) {
            fail("invalid TCP record length");
            return;
        }
        if (input.size() - offset < size_t(len + 2)) break;
        char plain[buf_len];
        int plain_len = len;
        if (my_decrypt(input.data() + offset + 2, plain, plain_len) != 0 || !process_record(plain, plain_len)) {
            fail("invalid or unauthenticated TCP record");
            return;
        }
        last_received = get_current_time();
        offset += len + 2;
        if (phase == tcp_phase::dead) return;
    }
    if (offset) input.erase(0, offset);
}

bool tcp_connection::process_record(char *plain, int len) {
    if (len < record_header_size || memcmp(plain + 16, protocol_magic, 4) != 0 || recv_sequence == 0 ||
        get_u64(plain + 53) != recv_sequence) return false;
    char type = plain[20];
    u32_t conv = read_u32(plain + 61);
    int data_len = len - record_header_size;
    ++recv_sequence;
    if (phase == tcp_phase::challenge) {
        if (type != 'C' || conv != 0 || data_len != 0 || !zero_nonce(plain + 37)) return false;
        memcpy(remote_nonce, plain + 21, nonce_size);
        phase = tcp_phase::accept;
        return control('H');
    }
    if (memcmp(plain + 37, local_nonce, nonce_size) != 0) return false;
    if (phase == tcp_phase::hello) {
        if (type != 'H' || conv != 0 || data_len != 0) return false;
        memcpy(remote_nonce, plain + 21, nonce_size);
        phase = tcp_phase::ready;
        return control('A');
    }
    if (memcmp(plain + 21, remote_nonce, nonce_size) != 0) return false;
    if (phase == tcp_phase::accept) {
        if (type != 'A' || conv != 0 || data_len != 0) return false;
        phase = tcp_phase::ready;
        mylog(log_info, "tcp tunnel ready%s\n", http_proxy_address.empty() ? "" : " through HTTP CONNECT proxy");
        return true;
    }
    if (!ready()) return false;
    if (type == 'P') return conv == 0 && data_len == 0;
    return type == 'D' && conv != 0 && receive_chunk(conv, plain + record_header_size, data_len);
}

bool tcp_connection::receive_chunk(u32_t conv, char *data, int len) {
    if (len < datagram_header_size) return false;
    unsigned total = read_u16(data), offset = read_u16(data + 2);
    unsigned count = len - datagram_header_size;
    if (total > datagram_limit || offset > total || count > total - offset || count > chunk_limit || (!count && total)) return false;
    if (offset == 0) {
        if (assembling) return false;
        assembling = true;
        incoming_conv = conv;
        incoming_size = total;
        assembly_started = get_current_time();
        datagram.clear();
    }
    if (!assembling || incoming_conv != conv || incoming_size != total || datagram.size() != offset) return false;
    datagram.append(data + datagram_header_size, count);
    if (datagram.size() == total) {
        deliver(conv, datagram.data(), total);
        assembling = false;
        datagram.clear();
    }
    return true;
}

void tcp_connection::deliver(u32_t conv, const char *data, int len) {
    if (program_mode == client_mode) {
        if (!context.client_peers.is_conv_used(conv)) return;
        address_t address = context.client_peers.find_data_by_conv(conv);
        context.client_peers.update_active_time(conv);
        if (sendto(context.local_fd, data, len, 0, (sockaddr *)&address.inner, address.get_len()) != len)
            mylog(log_warn, "tcp local UDP send failed: %s\n", get_sock_error());
        return;
    }
    auto found = peers.find(conv);
    if (found == peers.end()) {
        if (context.udp_count >= max_conv_num) {
            mylog(log_warn, "tcp UDP conversation limit reached\n");
            return;
        }
        int udp_fd = new_socket(remote_addr, SOCK_DGRAM);
        if (udp_fd < 0) {
            mylog(log_warn, "tcp UDP socket creation failed: %s\n", get_sock_error());
            return;
        }
        if (connect(udp_fd, (sockaddr *)&remote_addr.inner, remote_addr.get_len()) != 0) {
            mylog(log_warn, "tcp UDP connect failed: %s\n", get_sock_error());
            sock_close(udp_fd);
            return;
        }
        peers.emplace(conv, unique_ptr<udp_peer>(new udp_peer(*this, conv, udp_fd)));
        peer_lru.new_key(conv);
        found = peers.find(conv);
    } else {
        peer_lru.update(conv);
    }
    if (send(found->second->fd, data, len, 0) != len)
        mylog(log_warn, "tcp remote UDP send failed: %s\n", get_sock_error());
}

void tcp_connection::tick(u64_t now) {
    if (phase == tcp_phase::dead) return;
    if (!ready()) {
        if (now - started > client_handshake_timeout) fail("TCP/proxy handshake timeout");
        return;
    }
    if (now - last_received > client_conn_timeout) {
        fail("TCP heartbeat timeout");
        return;
    }
    if (assembling && now - assembly_started > client_handshake_timeout) {
        fail("UDP reassembly timeout");
        return;
    }
    if (now - last_sent >= heartbeat_interval) control('P');
    int budget = peer_lru.size() / conv_clear_ratio + conv_clear_min;
    while (budget-- && !peer_lru.empty()) {
        u32_t conv;
        if (now - peer_lru.peek_back(conv) < conv_timeout) break;
        peers.erase(conv);
        peer_lru.erase(conv);
    }
}

void tcp_context::connect_client() {
    address_t address = destinations[destination_index++ % destinations.size()];
    int fd = new_socket(address, SOCK_STREAM);
    next_connect = get_current_time() + client_retry_interval;
    if (fd < 0) {
        mylog(log_warn, "tcp socket creation failed: %s\n", get_sock_error());
        return;
    }
    int result = connect(fd, (sockaddr *)&address.inner, address.get_len());
    if (result != 0 && !connect_pending(get_sock_errno())) {
        mylog(log_warn, "tcp connect failed: %s\n", get_sock_error());
        sock_close(fd);
        return;
    }
    connections.emplace_back(new tcp_connection(*this, fd, tcp_phase::connecting));
    client = connections.back().get();
    if (result == 0) client->connected();
    else ev_io_start(loop, &client->writer);
}

void tcp_context::local_cb(struct ev_loop *, ev_io *watcher, int) {
    tcp_context &context = *(tcp_context *)watcher->data;
    for (int i = 0; i < io_batch_limit; ++i) {
        if (program_mode == server_mode) {
            int fd = socket_fd(accept(context.local_fd, NULL, NULL));
            if (fd < 0) {
                if (interrupted(get_sock_errno())) continue;
                return;
            }
            if (context.connections.size() >= max_ready_conn_num) {
                sock_close(fd);
                continue;
            }
            setnonblocking(fd);
            set_buf_size(fd, socket_buf_size);
            context.connections.emplace_back(new tcp_connection(context, fd, tcp_phase::hello));
            tcp_connection &connection = *context.connections.back();
            ev_io_start(context.loop, &connection.reader);
            connection.control('C');
            continue;
        }
        char data[65536];
        address_t source;
        socklen_t len = sizeof(source.inner);
        int count = recvfrom(context.local_fd, data, sizeof(data), 0, (sockaddr *)&source.inner, &len);
        if (count < 0) {
            int error = get_sock_errno();
            if (interrupted(error)) continue;
            if (!would_block(error)) mylog(log_warn, "tcp local UDP receive failed: %s\n", get_sock_error());
            return;
        }
        if (!context.client || !context.client->ready()) continue;
        u32_t conv;
        if (context.client_peers.is_data_used(source)) {
            conv = context.client_peers.find_conv_by_data(source);
            context.client_peers.update_active_time(conv);
        } else {
            if (context.client_peers.get_size() >= max_conv_num) {
                mylog(log_warn, "tcp local UDP conversation limit reached\n");
                continue;
            }
            conv = context.client_peers.get_new_conv();
            context.client_peers.insert_conv(conv, source);
        }
        context.client->send_datagram(conv, data, count);
    }
}

void tcp_context::timer_cb(struct ev_loop *, ev_timer *watcher, int) {
    tcp_context &context = *(tcp_context *)watcher->data;
    u64_t now = get_current_time();
    for (auto it = context.connections.begin(); it != context.connections.end();) {
        (*it)->tick(now);
        if ((*it)->phase == tcp_phase::dead) {
            if (it->get() == context.client) {
                context.client = NULL;
                context.next_connect = now + client_retry_interval;
            }
            it = context.connections.erase(it);
        } else ++it;
    }
    if (program_mode == client_mode) {
        context.client_peers.clear_inactive0(NULL);
        if (!context.client && now >= context.next_connect) context.connect_client();
    }
}
}  // namespace

void validate_tcp_options() {
    if ((!http_proxy_address.empty() || !http_proxy_credentials.empty()) && (raw_mode != mode_tcp || program_mode != client_mode)) {
        mylog(log_fatal, "HTTP proxy options require client mode and --raw-mode tcp\n");
        myexit(-1);
    }
    if (!http_proxy_address.empty()) {
        string host, port;
        if (!proxy_endpoint(host, port)) {
            mylog(log_fatal, "invalid --http-proxy: use host:port or http://host:port (bracket IPv6); HTTPS and URL credentials are unsupported\n");
            myexit(-1);
        }
    }
    if (!http_proxy_credentials.empty()) {
        size_t colon = http_proxy_credentials.find(':');
        bool valid = !http_proxy_address.empty() && colon != string::npos && colon != 0 && http_proxy_credentials.size() <= 1000;
        for (unsigned char c : http_proxy_credentials) if (c < 32 || c == 127) valid = false;
        if (!valid) {
            mylog(log_fatal, "--http-proxy-auth requires --http-proxy and user:password without control characters\n");
            myexit(-1);
        }
    }
    if (raw_mode == mode_tcp && (auto_add_iptables_rule || generate_iptables_rule || generate_iptables_rule_add || keep_rule ||
                                lower_level || use_tcp_dummy_socket || g_fix_gro || dev[0] || force_source_ip || force_source_port || fifo_file[0])) {
        mylog(log_fatal, "--raw-mode tcp cannot use raw firewall, lower-level, easy-tcp, fix-gro, dev, source or fifo options\n");
        myexit(-1);
    }
}

int tcp_event_loop() {
    tcp_context context(ev_default_loop(0));
    if (!context.loop) {
        mylog(log_fatal, "cannot create TCP event loop\n");
        return -1;
    }
    if (program_mode == client_mode) {
        if (http_proxy_address.empty()) context.destinations.push_back(remote_addr);
        else {
            string host, port;
            proxy_endpoint(host, port);
            addrinfo hints = {}, *results = NULL;
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICSERV;
            int error = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
            if (error != 0) {
                mylog(log_fatal, "HTTP proxy DNS resolution failed (code %d)\n", error);
                return -1;
            }
            for (addrinfo *entry = results; entry; entry = entry->ai_next) {
                if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) continue;
                address_t address;
                address.from_sockaddr(entry->ai_addr, (socklen_t)entry->ai_addrlen);
                context.destinations.push_back(address);
            }
            freeaddrinfo(results);
            if (context.destinations.empty()) return -1;
        }
    }
    context.local_fd = new_socket(local_addr, program_mode == client_mode ? SOCK_DGRAM : SOCK_STREAM);
    if (context.local_fd < 0) {
        mylog(log_fatal, "tcp mode local socket creation failed: %s\n", get_sock_error());
        return -1;
    }
#if !defined(__MINGW32__)
    if (program_mode == server_mode) {
        int one = 1;
        setsockopt(context.local_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
#endif
    if (::bind(context.local_fd, (sockaddr *)&local_addr.inner, local_addr.get_len()) != 0 ||
        (program_mode == server_mode && listen(context.local_fd, SOMAXCONN) != 0)) {
        mylog(log_fatal, "tcp mode bind/listen failed: %s\n", get_sock_error());
        sock_close(context.local_fd);
        return -1;
    }
    ev_io_init(&context.local_reader, tcp_context::local_cb, context.local_fd, EV_READ);
    context.local_reader.data = &context;
    ev_io_start(context.loop, &context.local_reader);
    ev_timer_init(&context.timer, tcp_context::timer_cb, 0, timer_interval / 1000.0);
    context.timer.data = &context;
    ev_timer_start(context.loop, &context.timer);
    mylog(log_info, "tcp mode %s listening at %s\n", program_mode == client_mode ? "UDP" : "TCP", local_addr.get_str());
    ev_run(context.loop, 0);
    ev_timer_stop(context.loop, &context.timer);
    ev_io_stop(context.loop, &context.local_reader);
    context.connections.clear();
    sock_close(context.local_fd);
    return 0;
}
