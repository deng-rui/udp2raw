#include "ordered_worker.h"
#include "packet_size.h"
#include "lib/aes-common.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef NDEBUG
#error "packet_tests requires assertions; compile with -UNDEBUG"
#endif

static void test_sizes() {
    assert(32 == encrypted_packet_size(16, cipher_aes128cbc, auth_none));
    assert(48 == encrypted_packet_size(16, cipher_aes128cbc, auth_md5));
    assert(52 == encrypted_packet_size(16, cipher_aes128cbc, auth_hmac_sha1));
    assert(36 == encrypted_packet_size(16, cipher_aes128cfb, auth_hmac_sha1));
    assert(24 == encrypted_packet_size(16, cipher_none, auth_simple));
    assert(20 == encrypted_packet_size(16, cipher_xor, auth_crc32));
    assert(-1 == encrypted_packet_size(-1, cipher_none, auth_none));
    assert(-1 == safer_packet_size(-1, cipher_none, auth_none, false, 20, 20));
    assert(20 == tcp_header_size(false, true));
    assert(28 == tcp_header_size(true, true));
    assert(32 == tcp_header_size(false, false));
    assert(40 == tcp_header_size(true, false));
    // IPv4、CBC+MD5、MTU 1280：默认 1177 字节 UDP，精简 TCP 头后 1193 字节。
    assert(1268 == safer_packet_size(1177 + 4, cipher_aes128cbc, auth_md5, false, 20, 32));
    assert(1284 == safer_packet_size(1178 + 4, cipher_aes128cbc, auth_md5, false, 20, 32));
    assert(1272 == safer_packet_size(1193 + 4, cipher_aes128cbc, auth_md5, false, 20, 20));
    assert(1288 == safer_packet_size(1194 + 4, cipher_aes128cbc, auth_md5, false, 20, 20));
    for (int cipher = 0; cipher < cipher_end; ++cipher) {
        for (int auth = 0; auth < auth_end; ++auth) {
            int previous = 0;
            for (int length = 18; length <= 1800; ++length) {
                int size = encrypted_packet_size(length, cipher_mode_t(cipher), auth_mode_t(auth));
                assert(size >= previous && size >= length);
                previous = size;
                assert(size + 74 == safer_packet_size(length - 18, cipher_mode_t(cipher), auth_mode_t(auth), true, 40, 32));
            }
        }
    }
    int value = -1;
    assert(parse_bounded_decimal("64", 0, 64, value) && value == 64);
    assert(parse_bounded_decimal("0", 0, 64, value) && value == 0);
    for (const char *bad : {"", "-1", "+1", "1x", " 1", "65", "99999999999999999999999"})
        assert(!parse_bounded_decimal(bad, 0, 64, value));
    assert(!parse_bounded_decimal("575", 576, 1800, value));
}

static void test_order_and_capacity() {
    std::atomic<bool> release(false);
    std::atomic<int> completed(0);
    ordered_worker_pool<int> pool(4, 4, [&](int &value) {
        if (value == 0) while (!release.load()) std::this_thread::yield();
        value += 10;
        ++completed;
    }, [] {});
    for (int i = 0; i < 4; ++i) assert(pool.try_submit(i));
    while (completed.load() != 3) std::this_thread::yield();
    int value = -1;
    assert(!pool.try_pop(value));
    assert(!pool.try_submit(4));
    release = true;
    pool.stop();
    for (int i = 0; i < 4; ++i) {
        assert(pool.try_pop(value));
        assert(i + 10 == value);
    }
    assert(!pool.try_pop(value));
    assert(!pool.try_submit(4));
    pool.stop();

    ordered_worker_pool<int> reused(3, 7, [](int &number) { number *= 3; }, [] {});
    int sent = 0, received = 0;
    while (received < 10000) {
        if (sent < 10000 && reused.try_submit(sent)) ++sent;
        while (reused.try_pop(value)) assert(received++ * 3 == value);
    }
}

static void test_aes_threads() {
    // NIST SP 800-38A AES-128-CBC 首块向量，独立于实现生成的期望值。
    const unsigned char key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    unsigned char plain[16] = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
    const unsigned char iv[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    const unsigned char expected[16] = {0x76,0x49,0xab,0xac,0x81,0x19,0xb2,0x46,0xce,0xe9,0x8e,0x9b,0x12,0xe9,0x19,0x7d};
    unsigned char encrypted[16];
    AES_CBC_encrypt_buffer(encrypted, plain, 16, key, iv);
    assert(0 == std::memcmp(expected, encrypted, 16));
    const unsigned char cfb_expected[16] = {0x3b,0x3f,0xd9,0x2e,0xb7,0x2d,0xad,0x20,0x33,0x34,0x49,0xf8,0xe8,0x3c,0xfb,0x4a};
    AES_CFB_encrypt_buffer(encrypted, plain, 16, key, iv);
    assert(0 == std::memcmp(cfb_expected, encrypted, 16));
    std::atomic<int> initialized(0);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([&, thread] {
            unsigned char local_key[16], input[16], result[16], decoded[16], cbc[16], cfb[16], ecb[16];
            std::memset(local_key, thread, 16);
            std::memcpy(input, plain, 16);
            AES_CBC_encrypt_buffer(cbc, input, 16, local_key, iv);
            AES_CFB_encrypt_buffer(cfb, input, 16, local_key, iv);
            AES_ECB_encrypt_buffer(input, local_key, ecb);
            AES_CBC_decrypt_buffer(decoded, cbc, 16, local_key, iv);
            AES_CFB_decrypt_buffer(decoded, cfb, 16, local_key, iv);
            AES_ECB_decrypt_buffer(ecb, local_key, decoded);
            ++initialized;
            while (initialized.load() != 8) std::this_thread::yield();
            for (int i = 0; i < 2000; ++i) {
                AES_CBC_encrypt_buffer(result, input, 16, nullptr, iv);
                assert(0 == std::memcmp(cbc, result, 16));
                AES_CBC_decrypt_buffer(decoded, result, 16, nullptr, iv);
                assert(0 == std::memcmp(input, decoded, 16));
                AES_CFB_encrypt_buffer(result, input, 16, nullptr, iv);
                assert(0 == std::memcmp(cfb, result, 16));
                AES_CFB_decrypt_buffer(decoded, result, 16, nullptr, iv);
                assert(0 == std::memcmp(input, decoded, 16));
                AES_ECB_encrypt_buffer(input, nullptr, result);
                assert(0 == std::memcmp(ecb, result, 16));
                AES_ECB_decrypt_buffer(result, nullptr, decoded);
                assert(0 == std::memcmp(input, decoded, 16));
            }
        });
    }
    for (auto &thread : threads) thread.join();
}

int main() {
    test_sizes();
    test_order_and_capacity();
    test_aes_threads();
    std::cout << "PASS: packet sizes, option bounds, queue ordering/capacity/reuse/shutdown, AES CBC/CFB/ECB threads\n";
}
