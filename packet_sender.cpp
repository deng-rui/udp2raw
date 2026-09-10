#include "common.h"
#include "misc.h"
#include "packet_sender.h"
#include "packet_size.h"
#include "ordered_worker.h"
#include <memory>

#ifdef UDP2RAW_LINUX
#include <sys/eventfd.h>
#endif

int packet_threads = 0;
int path_mtu = 0;
int compact_tcp = 0;

int safer_cipher_size(int length) {
    int size = encrypted_packet_size(length, cipher_mode, auth_mode);
    return size < 0 ? -1 : size + (g_fix_gro ? 2 : 0);
}

bool safer_fits_mtu(int length) {
    if (length < 0 || length + safer_header_size > max_data_len) return false;
    if (!path_mtu) return true;
    int transport = raw_mode == mode_faketcp ? tcp_header_size(false, compact_tcp) : 8;
    int size = safer_packet_size(length, cipher_mode, auth_mode, g_fix_gro,
                                 raw_ip_version == AF_INET ? 20 : 40, transport);
    // 接收路径也以 max_data_len 限制完整 IP 包。
    return size <= path_mtu;
}

void configure_packet_sender() {
#ifdef UDP2RAW_MP
    if (packet_threads) {
        mylog(log_fatal, "--threads requires the Linux raw-socket build\n");
        myexit(-1);
    }
#endif
    if ((packet_threads || path_mtu || compact_tcp) &&
        raw_mode != mode_faketcp && raw_mode != mode_udp && raw_mode != mode_icmp) {
        mylog(log_fatal, "--threads, --mtu and --compact-tcp require a raw-socket mode\n");
        myexit(-1);
    }
    if (compact_tcp && (raw_mode != mode_faketcp || use_tcp_dummy_socket)) {
        mylog(log_fatal, "--compact-tcp requires faketcp without --easy-tcp\n");
        myexit(-1);
    }
    if (path_mtu) {
        int payload = max_data_len - safer_header_size;
        while (payload >= 0 && !safer_fits_mtu(payload)) --payload;
        if (payload < conversation_header_size) {
            mylog(log_fatal, "--mtu leaves no UDP payload\n");
            myexit(-1);
        }
        if (hb_len > payload) hb_len = payload;
        mylog(log_info, "outer MTU=%d, maximum UDP payload=%d, heartbeat payload=%d\n",
              path_mtu, payload - conversation_header_size, hb_len);
    }
}

#ifdef UDP2RAW_LINUX
namespace {
const size_t sender_capacity = 256;
struct send_job {
    raw_info_t raw;
    char plain[buf_len];
    char encrypted[buf_len];
    int length = 0;
    int expected = 0;
    int result = -1;
};
std::unique_ptr<ordered_worker_pool<send_job>> sender;
int completion_fd = -1;
u64_t queue_drops = 0, send_errors = 0;

void notify_completion() {
    uint64_t one = 1;
    ssize_t result;
    do { result = write(completion_fd, &one, sizeof(one)); } while (result < 0 && errno == EINTR);
    // EAGAIN 表示计数器已可读；fd 只会在线程全部退出后关闭。
    if (result < 0 && errno != EAGAIN) std::terminate();
}
}

void init_packet_sender() {
    if (!packet_threads) return;
    completion_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (completion_fd < 0) {
        mylog(log_fatal, "eventfd failed: %s\n", strerror(errno));
        myexit(-1);
    }
    try {
        sender.reset(new ordered_worker_pool<send_job>(packet_threads, sender_capacity,
            [](send_job &job) {
                job.result = encrypt_safer_payload(job.plain, job.encrypted, job.length);
                if (job.result == 0 && job.length != job.expected) job.result = -1;
            }, notify_completion));
    } catch (const std::system_error &error) {
        mylog(log_fatal, "cannot start packet workers: %s\n", error.what());
        myexit(-1);
    }
    mylog(log_info, "packet encryption workers=%d, queue capacity=%zu\n", packet_threads, sender_capacity);
}

bool packet_sender_enabled() { return bool(sender); }
int packet_sender_fd() { return completion_fd; }

int queue_safer_packet(const raw_info_t &raw, const char *data, int length) {
    if (length < 0 || length > max_data_len) return -1;
    if (sender->try_submit_with([&](send_job &job) {
        job.raw = raw;
        job.length = length;
        job.expected = safer_cipher_size(length);
        memcpy(job.plain, data, length);
    })) return 0;
    // 按指数频率记录过载，避免逐包日志放大拥塞。
    ++queue_drops;
    if ((queue_drops & (queue_drops - 1)) == 0)
        mylog(log_warn, "packet worker queue full, dropped=%llu\n", queue_drops);
    return -1;
}

void drain_packet_sender() {
    uint64_t count;
    ssize_t result;
    do { result = read(completion_fd, &count, sizeof(count)); } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN) {
        mylog(log_fatal, "packet completion read failed: %s\n", strerror(errno));
        myexit(-1);
    }
    while (sender->try_consume([](send_job &job) {
        if (job.result != 0 || send_raw0(job.raw, job.encrypted, job.length) != 0) {
            ++send_errors;
            if ((send_errors & (send_errors - 1)) == 0)
                mylog(log_warn, "asynchronous packet send failed, total=%llu\n", send_errors);
        }
    })) {}
}

void stop_packet_sender() {
    if (sender) {
        sender->stop();
        sender.reset();
        mylog(log_info, "packet workers stopped, queue drops=%llu, send errors=%llu\n", queue_drops, send_errors);
    }
    if (completion_fd >= 0) {
        close(completion_fd);
        completion_fd = -1;
    }
}
#else
void init_packet_sender() {}
void stop_packet_sender() {}
bool packet_sender_enabled() { return false; }
int packet_sender_fd() { return -1; }
void drain_packet_sender() {}
int queue_safer_packet(const raw_info_t &, const char *, int) { return -1; }
#endif
