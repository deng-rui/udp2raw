#ifndef UDP2RAW_PACKET_SENDER_H
#define UDP2RAW_PACKET_SENDER_H

struct raw_info_t;
extern int packet_threads;
extern int path_mtu;
extern int compact_tcp;

void configure_packet_sender();
void init_packet_sender();
void stop_packet_sender();
int packet_sender_fd();
void drain_packet_sender();
bool packet_sender_enabled();
int queue_safer_packet(const raw_info_t &raw, const char *data, int length);
int safer_cipher_size(int length);
bool safer_fits_mtu(int length);

#endif
