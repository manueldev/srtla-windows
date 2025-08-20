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

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <stdio.h> // For fprintf and stderr
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
typedef unsigned long in_addr_t; // Define in_addr_t for Windows
#else
#include <sys/socket.h>
#include <endian.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

void exit_help() {
  print_help();
  exit(EXIT_FAILURE);
}

/* Return a textual representation of the last socket error. On Windows use
   WSAGetLastError(), otherwise errno */
const char *sock_err_str() {
#ifdef _WIN32
  static char buf[256];
  int e = WSAGetLastError();
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, e, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), NULL);
  return buf;
#else
  return strerror(errno);
#endif
}

int is_fatal_udp_error(int err) {
#ifdef _WIN32
  switch (err) {
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
    case WSAEADDRNOTAVAIL:
    case WSAECONNRESET:
      return 1;
    default:
      return 0;
  }
#else
  switch (err) {
    case ENETUNREACH:
    case EHOSTUNREACH:
    case EADDRNOTAVAIL:
    case ECONNRESET:
      return 1;
    default:
      return 0;
  }
#endif
}

/* Create a UDP socket and apply platform-specific hardening (Windows SIO_UDP_CONNRESET) */
int create_udp_socket() {
  int s;
#ifdef _WIN32
  s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) return -1;
  // Disable ICMP port unreachable generating WSAECONNRESET on recv/send
  // Some Windows toolchains may not define SIO_UDP_CONNRESET; only call when available.
  #ifdef SIO_UDP_CONNRESET
  DWORD bytes = 0;
  BOOL b = FALSE;
  WSAIoctl(s, SIO_UDP_CONNRESET, &b, sizeof(b), NULL, 0, &bytes, NULL, NULL);
  #else
  /* SIO_UDP_CONNRESET not defined on this platform; skipping WSAIoctl.
    This avoids compile failures on some MinGW/MSVCRT setups. */
  #endif
#else
  s = socket(AF_INET, SOCK_DGRAM, 0);
#endif
  return s;
}

#define ADDR_BUF_SZ 50
char _global_addr_buf[ADDR_BUF_SZ];
const char *print_addr(struct sockaddr *addr) {
  struct sockaddr_in *ain = (struct sockaddr_in *)addr;
  return inet_ntop(ain->sin_family, &ain->sin_addr, _global_addr_buf, ADDR_BUF_SZ);
}

int port_no(struct sockaddr *addr) {
  struct sockaddr_in *ain = (struct sockaddr_in *)addr;
  return ntohs(ain->sin_port);
}

int parse_ip(struct sockaddr_in *addr, char *ip_str) {
  in_addr_t ip = inet_addr(ip_str);
  if (ip == -1) return -1;

  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_addr.s_addr = ip;

  return 0;
}

int parse_port(char *port_str) {
  int port = strtol(port_str, NULL, 10);
  if (port <= 0 || port > 65535) return -2;
  return port;
}

#ifdef _WIN32
#include <windows.h>
#endif

int get_seconds(time_t *s) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    LARGE_INTEGER counter;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&counter);
    *s = (time_t)(counter.QuadPart / freq.QuadPart);
    return 0;
#else
  struct timespec ts;
  int ret = clock_gettime(CLOCK_MONOTONIC , &ts);
  if (ret != 0) return -1;
  *s = ts.tv_sec;
  return 0;
#endif
}

int get_ms(uint64_t *ms) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int initialized = 0;
    LARGE_INTEGER counter;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&counter);
    *ms = (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
    return 0;
#else
  struct timespec ts;
  int ret = clock_gettime(CLOCK_MONOTONIC , &ts);
  if (ret != 0) return -1;
  *ms = ((uint64_t)(ts.tv_sec)) * 1000 + ((uint64_t)(ts.tv_nsec)) / 1000 / 1000;

  return 0;
#endif
}

int32_t get_srt_sn(void *pkt, int n) {
  if (n < 4) return -1;

  uint32_t sn = be32toh(*((uint32_t *)pkt));
  if ((sn & (1 << 31)) == 0) {
    return (int32_t)sn;
  }

  return -1;
}

uint16_t get_srt_type(void *pkt, int n) {
  if (n < 2) return 0;
  return be16toh(*((uint16_t *)pkt));
}

int is_srt_ack(void *pkt, int n) {
  return get_srt_type(pkt, n) == SRT_TYPE_ACK;
}

int is_srtla_keepalive(void *pkt, int n) {
  return get_srt_type(pkt, n) == SRTLA_TYPE_KEEPALIVE;
}

int is_srtla_reg1(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG1_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG1;
}

int is_srtla_reg2(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG2_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG2;
}

int is_srtla_reg3(void *pkt, int len) {
  if (len != SRTLA_TYPE_REG3_LEN) return 0;
  return get_srt_type(pkt, len) == SRTLA_TYPE_REG3;
}

// Ensure WSAStartup is initialized for Windows
#ifdef _WIN32
void initialize_winsock() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }
}
#endif
