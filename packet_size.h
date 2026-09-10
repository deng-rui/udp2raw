#ifndef UDP2RAW_PACKET_SIZE_H
#define UDP2RAW_PACKET_SIZE_H

#include "encrypt.h"

const int safer_header_size = 18;
const int conversation_header_size = 4;

// HMAC 模式先加密再认证，其余旧模式先认证再加密；CBC 必须保留一字节填充。
inline int encrypted_packet_size(int plain, cipher_mode_t cipher, auth_mode_t auth) {
    if (plain < 0 || plain > 65500 || cipher < cipher_none || cipher >= cipher_end) return -1;
    int tag;
    switch (auth) {
        case auth_none: tag = 0; break;
        case auth_md5: tag = 16; break;
        case auth_crc32: tag = 4; break;
        case auth_simple: tag = 8; break;
        case auth_hmac_sha1: tag = 20; break;
        default: return -1;
    }
    int length = plain + (auth == auth_hmac_sha1 ? 0 : tag);
    if (cipher == cipher_aes128cbc) length = (length / 16 + 1) * 16;
    return length + (auth == auth_hmac_sha1 ? tag : 0);
}

inline int tcp_header_size(bool syn, bool compact) {
    return compact ? (syn ? 28 : 20) : (syn ? 40 : 32);
}

inline int safer_packet_size(int payload, cipher_mode_t cipher, auth_mode_t auth,
                             bool gro, int ip_header, int transport_header) {
    if (payload < 0 || payload > 65000) return -1;
    int length = encrypted_packet_size(payload + safer_header_size, cipher, auth);
    return length < 0 ? -1 : length + (gro ? 2 : 0) + ip_header + transport_header;
}

inline bool parse_bounded_decimal(const char *text, int minimum, int maximum, int &value) {
    if (!text || !*text) return false;
    int result = 0;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || result > (maximum - (*p - '0')) / 10) return false;
        result = result * 10 + (*p - '0');
        if (result > maximum) return false;
    }
    if (result < minimum) return false;
    value = result;
    return true;
}
#endif
