#ifndef UDP2RAW_TCP_H_
#define UDP2RAW_TCP_H_

#include <string>

extern std::string http_proxy_address;
extern std::string http_proxy_credentials;

void validate_tcp_options();
int tcp_event_loop();

#endif
