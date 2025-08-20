/*
    srtla - SRT transport proxy with link aggregation
    Copyright (C) 2020-2021 BELABOX project

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdint.h>

#define MTU 1500

#define SRT_TYPE_HANDSHAKE   0x8000
#define SRT_TYPE_ACK         0x8002
#define SRT_TYPE_NAK         0x8003
#define SRT_TYPE_SHUTDOWN    0x8005

#define SRTLA_TYPE_KEEPALIVE 0x9000
#define SRTLA_TYPE_ACK       0x9100
#define SRTLA_TYPE_REG1      0x9200
#define SRTLA_TYPE_REG2      0x9201
#define SRTLA_TYPE_REG3      0x9202
#define SRTLA_TYPE_REG_ERR   0x9210
#define SRTLA_TYPE_REG_NGP   0x9211
#define SRTLA_TYPE_REG_NAK   0x9212

#define SRT_MIN_LEN          16

#define SRTLA_ID_LEN         256
#define SRTLA_TYPE_REG1_LEN  (2 + (SRTLA_ID_LEN))
#define SRTLA_TYPE_REG2_LEN  (2 + (SRTLA_ID_LEN))
#define SRTLA_TYPE_REG3_LEN  2

typedef struct __attribute__((__packed__)) {
  uint16_t type;
  uint16_t subtype;
  uint32_t info;
  uint32_t timestamp;
  uint32_t dest_id;
} srt_header_t;

typedef struct __attribute__((__packed__)) {
  srt_header_t header;
  uint32_t version;
  uint16_t enc_field;
  uint16_t ext_field;
  uint32_t initial_seq;
  uint32_t mtu;
  uint32_t mfw;
  uint32_t handshake_type;
  uint32_t source_id;
  uint32_t syn_cookie;
  char     peer_ip[16];
} srt_handshake_t;

/* New states and connection enums for reconnection/resilience */
typedef enum {
  G_ACTIVE = 0,
  G_WAITING_SRT,
  G_CLOSED
} group_state;

typedef enum {
  C_CONNECTING = 0,
  C_REGISTERED,
  C_RECONNECTING,
  C_DEAD
} conn_state;

/* Reconnection and retry defaults (can be overridden at runtime) */
#ifndef REG_RETRY_BASE_MS
#define REG_RETRY_BASE_MS 250
#endif
#ifndef REG_RETRY_MAX_MS
#define REG_RETRY_MAX_MS 4000
#endif
#ifndef REG_RETRY_MAX_ATTEMPTS
#define REG_RETRY_MAX_ATTEMPTS 8
#endif

#ifndef CONN_DEAD_NO_ACTIVITY_MS
#define CONN_DEAD_NO_ACTIVITY_MS 5000
#endif

/* Helper for more explanatory socket error strings */
const char *sock_err_str();
int is_fatal_udp_error(int err);
int create_udp_socket(void);


#define LOG_NONE    0   // prints only fatal errors
#define LOG_ERR     1   // prints errors we can tolerate
#define LOG_INFO    2   // prints informational messages
#define LOG_DEBUG   3   // prints potentially verbose messages about the internal workings

#define LOG_LEVEL LOG_INFO

#if LOG_LEVEL >= LOG_DEBUG
  #define debug(...) fprintf(stderr, __VA_ARGS__)
#else
  #define debug(...)
#endif

#if LOG_LEVEL >= LOG_INFO
  #define info(...) fprintf(stderr, __VA_ARGS__)
#else
  #define info(...)
#endif

#if LOG_LEVEL >= LOG_ERR
  #define err(...) fprintf(stderr, __VA_ARGS__)
#else
  #define err(...)
#endif

void print_help();
void exit_help();

int get_seconds(time_t *s);
int get_ms(uint64_t *ms);

const char *print_addr(struct sockaddr *addr);
int port_no(struct sockaddr *addr);
int parse_ip(struct sockaddr_in *addr, char *ip_str);
int parse_port(char *port_str);

int32_t get_srt_sn(void *pkt, int n);
uint16_t get_srt_type(void *pkt, int n);
int is_srt_ack(void *pkt, int n);
int is_srt_shutdown(void *pkt, int n);

int is_srtla_keepalive(void *pkt, int len);
int is_srtla_reg1(void *pkt, int len);
int is_srtla_reg2(void *pkt, int len);
int is_srtla_reg3(void *pkt, int len);

#ifdef _WIN32
// Windows için byte order makroları
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef htobe16
#define htobe16(x) htons(x)
#endif
#ifndef be32toh
#define be32toh(x) ntohl(x)
#endif
#ifndef htobe32
#define htobe32(x) htonl(x)
#endif
#ifndef be16toh
#define be16toh(x) ntohs(x)
#endif
#endif
