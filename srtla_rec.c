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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> 
#include <time.h> 
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
typedef unsigned long in_addr_t; // Define in_addr_t for Windows

// Update sendto, recv, and recvfrom calls for Winsock compatibility
#define SENDTO(sock, buf, len, flags, addr, addrlen) sendto(sock, (char *)(buf), len, flags, addr, addrlen)
#define RECV(sock, buf, len, flags) recv(sock, (char *)(buf), len, flags)
#define RECVFROM(sock, buf, len, flags, addr, addrlen) recvfrom(sock, (char *)(buf), len, flags, addr, addrlen)
#else
#include <sys/socket.h>
#include <endian.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define SENDTO(sock, buf, len, flags, addr, addrlen) sendto(sock, buf, len, flags, addr, addrlen)
#define RECV(sock, buf, len, flags) recv(sock, buf, len, flags)
#define RECVFROM(sock, buf, len, flags, addr, addrlen) recvfrom(sock, buf, len, flags, addr, addrlen)
#endif
#ifndef _WIN32
#include <netdb.h>
#endif
#include <sys/types.h>
#ifdef __linux__
#include <sys/epoll.h>
#endif
#include <errno.h>

#include "common.h"

#define MAX_CONNS_PER_GROUP 8
#define MAX_GROUPS          200

#define CLEANUP_PERIOD 3
#define GROUP_TIMEOUT  10
#define CONN_TIMEOUT   10

#define RECV_ACK_INT 10
typedef struct srtla_conn {
  struct srtla_conn *next;
  struct sockaddr addr;
  time_t last_rcvd;
  int recv_idx;
  uint32_t recv_log[RECV_ACK_INT];
  /* registration / reconnect state */
  int reg_attempts;
  time_t next_reg_try_ms;
  int backoff_ms;
  int had_fatal_error;
} conn_t;

typedef struct srtla_conn_group {
  struct srtla_conn_group *next;
  conn_t *conns;
  time_t created_at;
  int srt_sock;
  struct sockaddr last_addr;
  char id[SRTLA_ID_LEN];
  /* reconnection state */
  uint64_t logical_group_id;
  group_state state;
  time_t next_srt_retry_ms;
  int srt_retry_attempts;
} conn_group_t;

typedef struct {

/*
Manual testing scenarios:

1) Start receiver + SRT listener + sender. Verify streaming flows normally.
2) Stop SRT listener (e.g., OBS). Verify receiver logs entering WAITING_SRT and schedules retries.
3) Restart SRT listener. Verify receiver transitions back to ACTIVE automatically.
4) On sender, disable a network interface and confirm only that connection reconnects while others remain.
5) Force ICMP Port Unreachable on Windows (close receiver port briefly) and confirm sockets are recreated and registration retried.
6) Repeatedly bring down/up SRT listener and confirm no unbounded memory growth (use counters/logs).

*/
  uint32_t type;
  uint32_t acks[RECV_ACK_INT];
} srtla_ack_pkt;


int srtla_sock;
struct sockaddr srt_addr;
const socklen_t addr_len = sizeof(struct sockaddr);

conn_group_t *groups = NULL;
int group_count = 0;
static uint64_t global_group_seq = 1;

/* runtime flags */
int flag_auto_reconnect = 1;
int flag_log_errors = 0;
int flag_reconnect_interval_ms = 500;

FILE *urandom;

/*

Async I/O support

*/
#ifdef __linux__
int socket_epoll;

int epoll_add(int fd, uint32_t events, void *userdata) {
  struct epoll_event ev={0};
  ev.events = events;
  ev.data.ptr = userdata;
  return epoll_ctl(socket_epoll, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_rem(int fd) {
  struct epoll_event ev; // non-NULL for Linux < 2.6.9, however unlikely it is
  return epoll_ctl(socket_epoll, EPOLL_CTL_DEL, fd, &ev);
}
#endif

/*

Misc helper functions

*/
void print_help() {
  fprintf(stderr,
          "Syntax: srtla_rec [-v] SRTLA_LISTEN_PORT SRT_HOST SRT_PORT\n\n"
          "-v      Print the version and exit\n");
}

int const_time_cmp(const void *a, const void *b, int len) {
  char diff = 0;
  char *ca = (char *)a;
  char *cb = (char *)b;
  for (int i = 0; i < len; i++) {
    diff |= *ca - *cb;
    ca++;
    cb++;
  }

  return diff ? -1 : 0;
}

int get_random(void *dest, size_t len) {
#ifdef _WIN32
  // Windows'ta CryptGenRandom kullanarak rastgele sayı üret
  HCRYPTPROV hCryptProv;
  if (!CryptAcquireContext(&hCryptProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
    fprintf(stderr, "CryptAcquireContext failed: %lu\n", GetLastError());
    return -1;
  }
  
  BOOL result = CryptGenRandom(hCryptProv, (DWORD)len, (BYTE*)dest);
  CryptReleaseContext(hCryptProv, 0);
  
  return result ? 0 : -1;
#else
  while (len) {
    int ret = fread(dest, 1, len, urandom);
    if (ret <= 0) return -1;
    len -= ret;
  }
  return 0;
#endif
}


/*

Connection and group management functions

*/
conn_group_t *group_find_by_id(char *id) {
  for (conn_group_t* g = groups; g != NULL; g = g->next) {
    if (const_time_cmp(g->id, id, SRTLA_ID_LEN) == 0) {
      return g;
    }
  }

  return NULL;
}

int group_find_by_addr(struct sockaddr *addr, conn_group_t **rg, conn_t **rc) {
  for (conn_group_t* g = groups; g != NULL; g = g->next) {
    for (conn_t *c = g->conns; c != NULL; c = c->next) {
      if (const_time_cmp(&(c->addr), addr, addr_len) == 0) {
        *rg = g;
        *rc = c;
        return 1;
      }
    }
    if (const_time_cmp(&g->last_addr, addr, addr_len) == 0) {
      *rg = g;
      *rc = NULL;
      return 0;
    }
  }

  return -1;
}

conn_group_t *group_create(char *sender_id, time_t ts) {
  // Make sure the ID isn't a duplicate - very unlikely
  char id[SRTLA_ID_LEN];
  memcpy(&id, sender_id, SRTLA_ID_LEN/2);
  do {
    int ret = get_random(&id[SRTLA_ID_LEN/2], SRTLA_ID_LEN/2);
    if (ret != 0) return NULL;
  } while(group_find_by_id(id) != NULL);

  // Allocate the new group
  conn_group_t *g = malloc(sizeof(conn_group_t));
  if (g == NULL) {
    err("malloc() failed\n");
    return NULL;
  }

  // And initialize it with the ID we've built above
  memcpy(&g->id, id, SRTLA_ID_LEN);
  g->conns = NULL;
  g->srt_sock = -1;
  g->logical_group_id = global_group_seq++;
  g->state = G_ACTIVE;
  g->next_srt_retry_ms = 0;
  g->srt_retry_attempts = 0;
  g->created_at = ts;
  g->next = groups;
  groups = g;

  return g;
}

int group_destroy(conn_group_t *g, conn_group_t **prev_link) {
  if (g == NULL) return -1;

  for (conn_t *c = g->conns; c != NULL;) {
    conn_t *next = c->next;
    free(c);
    c = next;
  }

  if (g->srt_sock > 0) {
#ifdef __linux__
    epoll_rem(g->srt_sock);
#endif
    close(g->srt_sock);
  }

  if (prev_link != NULL) {
    // The caller passed us a pointer to the linked list pointer to this group
    *prev_link = g->next;
  } else {
    // Search and unlink
    for (conn_group_t **it = &groups; (*it) != NULL; it = &((*it)->next)) {
      if (*it == g) {
        *it = g->next;
        break;
      }
    } // for
  } // prev_link == NULL

  free(g);

  /* Must ensure statements updating group_count on the creation and
     destruction code paths match up so we don't drift */
  group_count--;

  return 0;
}

int group_count_conns(conn_group_t *g) {
  int count = 0;
  for (conn_t *c = g->conns; c != NULL; c = c->next) {
    count++;
  }
  return count;
}

int group_reg(struct sockaddr *addr, char *in_buf, time_t ts) {
  if (group_count >= MAX_GROUPS) {
    err("%s:%d: group count is %d, rejecting group registration\n",
        print_addr(addr), port_no(addr), group_count);
    goto err;
  }

  // If this remote address is already registered, abort
  conn_group_t *g = NULL;
  conn_t *c;
  int ret = group_find_by_addr(addr, &g, &c);
  if (ret != -1) goto err;

  // Allocate the group
  char *id = in_buf + 2;
  g = group_create(id, ts);
  if (g == NULL) goto err;

  /* Record the address used to register the group
     It won't be allowed to register another group while this one is active */
  g->last_addr = *addr;

  // Build a REG2 packet
  char out_buf[SRTLA_TYPE_REG2_LEN];
  uint16_t header = htobe16(SRTLA_TYPE_REG2);
  memcpy(out_buf, &header, sizeof(header));
  memcpy(out_buf + sizeof(header), g->id, SRTLA_ID_LEN);

  // Send the REG2 packet
  ret = SENDTO(srtla_sock, out_buf, sizeof(out_buf), 0, addr, addr_len);
  if (ret != sizeof(out_buf)) goto err_destroy;

  info("%s:%d: group #%llu registered\n", print_addr(addr), port_no(addr), (unsigned long long)g->logical_group_id);

  // Only count the group after everything else succeeded
  group_count++;

  return 0;

err_destroy:
  groups = g->next;
  free(g);

err:
  err("%s:%d: group registration failed\n", print_addr(addr), port_no(addr));
  header = htobe16(SRTLA_TYPE_REG_ERR);
  SENDTO(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
  return -1;
}

int conn_reg(struct sockaddr *addr, char *in_buf, time_t ts) {
  conn_group_t *g, *tmp;
  conn_t *c;

  char *id = in_buf + 2;
  g = group_find_by_id(id);
  if (g == NULL) {
    uint16_t header = htobe16(SRTLA_TYPE_REG_NGP);
    SENDTO(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
    goto err_early;
  }

  /* If the connection is already registered, we'll allow it to register
     again to the same group, but not to a new one */
  int ret = group_find_by_addr(addr, &tmp, &c);
  if (ret != -1 && tmp != g) goto err;

  /* If the connection is already registered to the group, we can
     just skip ahead to sending the SRTLA_REG3 */
  if (ret != 1) {
    int conn_count = group_count_conns(g);
    if (conn_count >= MAX_CONNS_PER_GROUP) goto err;

    c = malloc(sizeof(conn_t));
    if (c == NULL) {
      err("malloc() failed\n");
      goto err;
    }
    c->addr = *addr;
    c->recv_idx = 0;
    c->last_rcvd = ts;
    c->next = g->conns;
    g->conns = c;
  }

  uint16_t header = htobe16(SRTLA_TYPE_REG3);
  ret = SENDTO(srtla_sock, &header, sizeof(header), 0, addr, addr_len);
  if (ret != sizeof(header)) goto err_destroy;

  info("%s:%d (group %p): connection registration\n", print_addr(addr), port_no(addr), g);

  // If it all worked, mark this peer as the most recently active one
  g->last_addr = *addr;

  return 0;

err_destroy:
  g->conns = c->next;
  free(c);

err:
  header = htobe16(SRTLA_TYPE_REG_ERR);
  SENDTO(srtla_sock, &header, sizeof(header), 0, addr, addr_len);

err_early:
  err("%s:%d: connection registration for group %p failed\n",
      print_addr(addr), port_no(addr), g);
  return -1;
}

/*

The main network event handlers

Resource limits:
  * connections per group MAX_CONNS_PER_GROUP
  * total groups          MAX_GROUPS

*/

void handle_srt_data(conn_group_t *g) {
  char buf[MTU];

  if (g == NULL) return;

  int n = RECV(g->srt_sock, &buf, MTU, 0);
  if (n < SRT_MIN_LEN) {
    int e = errno;
    if (flag_log_errors) err("Group #%llu (ptr=%p): SRT read failed (err=%s). Entering WAITING_SRT\n", (unsigned long long)g->logical_group_id, g, sock_err_str());
    else err("Group %p: failed to read the SRT sock, entering WAITING_SRT\n", g);
    // Close socket and mark for retry rather than destroying the whole group
    if (g->srt_sock > 0) { close(g->srt_sock); }
    g->srt_sock = -1;
    if (flag_auto_reconnect) {
      g->state = G_WAITING_SRT;
      g->srt_retry_attempts++;
      g->next_srt_retry_ms = time(NULL) * 1000 + flag_reconnect_interval_ms * (1 << (g->srt_retry_attempts - 1));
      if (g->next_srt_retry_ms - (time(NULL)*1000) > REG_RETRY_MAX_MS) {
        g->next_srt_retry_ms = time(NULL)*1000 + REG_RETRY_MAX_MS;
      }
    } else {
      group_destroy(g, NULL);
    }
    return;
  }

  // ACK
  if (is_srt_ack(buf, n)) {
    // Broadcast SRT ACKs over all connections for timely delivery
    for (conn_t *c = g->conns; c != NULL; c = c->next) {
      int ret = SENDTO(srtla_sock, &buf, n, 0, &c->addr, addr_len);
      if (ret != n) {
    if (flag_log_errors) err("%s:%d (group #%llu): failed to send the SRT ack (ret=%d, err=%s)\n",
      print_addr(&c->addr), port_no(&c->addr), (unsigned long long)g->logical_group_id, ret, sock_err_str());
    else err("%s:%d (group %p): failed to send the SRT ack\n",
      print_addr(&c->addr), port_no(&c->addr), g);
      }
    }
  } else {
    // send other packets over the most recently used SRTLA connection
    int ret = SENDTO(srtla_sock, &buf, n, 0, &g->last_addr, addr_len);
    if (ret != n) {
      int serr = errno;
      if (flag_log_errors) err("%s:%d (group #%llu): failed to send the SRT packet (ret=%d, err=%s)\n",
          print_addr(&g->last_addr), port_no(&g->last_addr), (unsigned long long)g->logical_group_id, ret, sock_err_str());
      else err("%s:%d (group %p): failed to send the SRT packet\n",
          print_addr(&g->last_addr), port_no(&g->last_addr), g);
      // If fatal, mark connection for reconnect
      if (is_fatal_udp_error(serr)) {
        // remove the connection immediately
        for (conn_t **it = &g->conns; *it != NULL; it = &((*it)->next)) {
          if (const_time_cmp(&((*it)->addr), &g->last_addr, addr_len) == 0) {
            conn_t *dead = *it;
            *it = dead->next;
            free(dead);
            break;
          }
        }
      }
    }
  }
}

void register_packet(conn_group_t *g, conn_t *c, int32_t sn) {
  // store the sequence numbers in BE, as they're transmitted over the network
  c->recv_log[c->recv_idx++] = htobe32(sn);

  if (c->recv_idx == RECV_ACK_INT) {
    srtla_ack_pkt ack;
    ack.type = htobe32(SRTLA_TYPE_ACK << 16);
    memcpy(&ack.acks, &c->recv_log, sizeof(c->recv_log));

    int ret = SENDTO(srtla_sock, &ack, sizeof(ack), 0, &c->addr, addr_len);
    if (ret != sizeof(ack)) {
      err("%s:%d (group %p): failed to send the srtla ack\n",
          print_addr(&c->addr), port_no(&c->addr), g);
    }

    c->recv_idx = 0;
  }
}

void handle_srtla_data(time_t ts) {
  char buf[MTU];
  int ret;

  // Get the packet
  struct sockaddr srtla_addr;
  socklen_t len = addr_len;
  int n = recvfrom(srtla_sock, buf, MTU, 0, &srtla_addr, &len);
  if (n < 0) {
    err("Failed to read a srtla packet\n");
    return;
  }

  // Handle srtla registration packets
  if (is_srtla_reg1(buf, n)) {
    group_reg(&srtla_addr, buf, ts);
    return;
  }

  if (is_srtla_reg2(buf, n)) {
    conn_reg(&srtla_addr, buf, ts);
    return;
  }

  // Check that the peer is a member of a connection group, discard otherwise
  conn_t *c;
  conn_group_t *g;
  ret = group_find_by_addr(&srtla_addr, &g, &c);
  if (ret != 1) return;

  // Update the connection's use timestamp
  c->last_rcvd = ts;

  // Resend SRTLA keep-alive packets to the sender
  if (is_srtla_keepalive(buf, n)) {
    int ret = SENDTO(srtla_sock, &buf, n, 0, &srtla_addr, addr_len);
    if (ret != n) {
      err("%s:%d (group %p): failed to send the srtla keepalive\n",
          print_addr(&srtla_addr), port_no(&srtla_addr), g);
    }
    return;
  }

  // Check that the packet is large enough to be an SRT packet, discard otherwise
  if (n < SRT_MIN_LEN) return;

  // Record the most recently active peer
  g->last_addr = srtla_addr;

  // Keep track of the received data packets to send SRTLA ACKs
  int32_t sn = get_srt_sn(buf, n);
  if (sn >= 0) {
    register_packet(g, c, sn);
  }

  // Open a connection to the SRT server for the group
  if (g->srt_sock < 0) {
    int sock = create_udp_socket();
    if (sock < 0) {
      err("Group #%llu: failed to create an SRT socket (%s)\n", (unsigned long long)g->logical_group_id, sock_err_str());
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        g->srt_retry_attempts++;
        uint64_t now = 0; get_ms(&now);
        int backoff = min(flag_reconnect_interval_ms << (g->srt_retry_attempts - 1), REG_RETRY_MAX_MS);
        g->next_srt_retry_ms = now + backoff;
        return;
      }
      group_destroy(g, NULL);
      return;
    }
    g->srt_sock = sock;

    int ret = connect(sock, &srt_addr, addr_len);
    if (ret != 0) {
      err("Group #%llu: failed to connect() the SRT socket (%s)\n", (unsigned long long)g->logical_group_id, sock_err_str());
      close(sock);
      g->srt_sock = -1;
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        g->srt_retry_attempts++;
        uint64_t now = 0; get_ms(&now);
        int backoff = min(flag_reconnect_interval_ms << (g->srt_retry_attempts - 1), REG_RETRY_MAX_MS);
        g->next_srt_retry_ms = now + backoff;
        return;
      }
      group_destroy(g, NULL);
      return;
    }

#ifdef __linux__
    ret = epoll_add(sock, EPOLLIN, g);
    if (ret < 0) {
      err("Group #%llu: failed to add the SRT socket to the epoll\n", (unsigned long long)g->logical_group_id);
      close(sock);
      g->srt_sock = -1;
      if (flag_auto_reconnect) {
        g->state = G_WAITING_SRT;
        uint64_t now = 0; get_ms(&now);
        g->next_srt_retry_ms = now + flag_reconnect_interval_ms;
        return;
      }
      group_destroy(g, NULL);
      return;
    }
#endif
  }

  ret = send(g->srt_sock, buf, n, 0);
  if (ret != n) {
    err("Group %p: failed to forward the srtla packet, terminating the group\n", g);
    group_destroy(g, NULL);
  }
}

/*
  Freeing resources

  Groups:
    * new groups with no connection: created_at < (ts - G_TIMEOUT)
    * other groups: when all connections have timed out
  Connections:
    * GC last_rcvd < (ts - CONN_TIMEOUT)
*/
void connection_cleanup(time_t ts) {
  static time_t last_ran = 0;
  if ((last_ran + CLEANUP_PERIOD) > ts) return;
  last_ran = ts;

  if (groups == NULL) return;

  int total_groups = 0, total_conns = 0, removed_groups = 0, removed_conns = 0;

  debug("Started a cleanup run\n");

  conn_group_t *next_g = NULL;
  conn_group_t **prev_g = &groups;
  for (conn_group_t *g = groups; g != NULL; g = next_g) {
    total_groups++;
    next_g = g->next;

    conn_t *next_c = NULL;
    conn_t **prev_c = &g->conns;
    for (conn_t *c = g->conns; c != NULL; c = next_c) {
      total_conns++;
      next_c = c->next;
      if ((c->last_rcvd + CONN_TIMEOUT) < ts) {
        removed_conns++;
        info("%s:%d (group %p): connection removed (timed out)\n",
             print_addr(&c->addr), port_no(&c->addr), g);
        *prev_c = next_c;
        free(c);
        continue;
      }
      prev_c = &c->next;
    }

    if (g->conns == NULL && (g->created_at + GROUP_TIMEOUT) < ts) {
      removed_groups++;
      info("Group %p: removed (no connections)\n", g);
      group_destroy(g, prev_g);
      continue;
    }
    prev_g = &g->next;
  }

  /* Also attempt SRT re-handshake for groups in WAITING_SRT state */
  for (conn_group_t *g = groups; g != NULL; g = g->next) {
    if (g->state == G_WAITING_SRT) {
      uint64_t now_ms = 0;
      uint64_t tmp = 0;
      if (get_ms(&tmp) == 0) now_ms = tmp;
      if (now_ms >= (uint64_t)g->next_srt_retry_ms) {
        // Try to handshake with the existing srt_addr
        info("Group #%llu: retrying SRT handshake attempt %d\n", (unsigned long long)g->logical_group_id, g->srt_retry_attempts);
        srt_handshake_t hs_packet = {0};
        hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
        hs_packet.version = htobe32(4);
        hs_packet.ext_field = htobe16(2);
        hs_packet.handshake_type = htobe32(1);

        int sock = create_udp_socket();
        if (sock >= 0) {
          struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
#ifdef _WIN32
          setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
          setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
          if (connect(sock, &srt_addr, addr_len) == 0) {
            int sent = send(sock, (const char*)&hs_packet, sizeof(hs_packet), 0);
            if (sent == sizeof(hs_packet)) {
              char buf[MTU];
              int r = recv(sock, buf, MTU, 0);
              if (r == sizeof(hs_packet)) {
                // success
                g->srt_sock = sock;
                g->state = G_ACTIVE;
                g->srt_retry_attempts = 0;
                info("Group #%llu: SRT handshake succeeded, group ACTIVE\n", (unsigned long long)g->logical_group_id);
                // continue to next group
                continue;
              }
            }
          }
          close(sock);
        }

        if (g->state != G_ACTIVE) {
          g->srt_retry_attempts++;
          int backoff = REG_RETRY_BASE_MS << (g->srt_retry_attempts - 1);
          if (backoff > REG_RETRY_MAX_MS) backoff = REG_RETRY_MAX_MS;
          uint64_t now = 0;
          get_ms(&now);
          g->next_srt_retry_ms = now + backoff;
          info("Group #%llu: scheduling next SRT retry in %d ms\n", (unsigned long long)g->logical_group_id, backoff);
        }
      }
    }
  }

  debug("Clean up run ended. Counted %d groups and %d connections. "
        "Removed %d groups and %d connections\n",
        total_groups, total_conns, removed_groups, removed_conns);
}

/*
SRT is connection-oriented and it won't reply to our packets at this point
unless we start a handshake, so we do that for each resolved address

Returns: -1 when an error has been encountered
          0 when the address was resolved but SRT appears unreachable
          1 when the address was resolved and SRT appears reachable
*/
int resolve_srt_addr(char *host, char *port) {
  // Let's set up an SRT handshake induction packet
  srt_handshake_t hs_packet = {0};
  hs_packet.header.type = htobe16(SRT_TYPE_HANDSHAKE);
  hs_packet.version = htobe32(4);
  hs_packet.ext_field = htobe16(2);
  hs_packet.handshake_type = htobe32(1);

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *srt_addrs;
  int ret = getaddrinfo(host, port, &hints, &srt_addrs);
  if (ret != 0) {
    fprintf(stderr, "Failed to resolve the address %s:%s\n", host, port);
    return -1;
  }

  int tmp_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (tmp_sock < 0) {
    perror("failed to create a UDP socket");
    return -1;
  }
  struct timeval to = { .tv_sec = 1, .tv_usec = 0};
#ifdef _WIN32
  ret = setsockopt(tmp_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
  ret = setsockopt(tmp_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
#endif
  if (ret != 0) {
    perror("failed to set a socket timeout");
    return -1;
  }

  int found = -1;
  for (struct addrinfo *addr = srt_addrs; addr != NULL && found == -1; addr = addr->ai_next) {
    info("Trying to connect to SRT at %s:%s... ", print_addr(addr->ai_addr), port);
    /* We're not printing this at all log levels, but a
       flush won't hurt if we didn't print anything */
    fflush(stderr);    ret = connect(tmp_sock, addr->ai_addr, addr->ai_addrlen);
    if (ret == 0) {
#ifdef _WIN32
      ret = send(tmp_sock, (const char*)&hs_packet, sizeof(hs_packet), 0);
#else
      ret = send(tmp_sock, &hs_packet, sizeof(hs_packet), 0);
#endif
      if (ret == sizeof(hs_packet)) {        char buf[MTU];
        ret = recv(tmp_sock, buf, MTU, 0);
        if (ret == sizeof(hs_packet)) {
          info("success\n");
          srt_addr = *addr->ai_addr;
          found = 1;
        }
      } // ret == sizeof(buf)
    } // ret == 0

    if (found == -1) {
      info("error\n");
    }
  }
  close(tmp_sock);

  if (found == -1) {
    srt_addr = *srt_addrs->ai_addr;
    fprintf(stderr, "WARNING: Failed to confirm that a SRT server is reachable at any address\n"
                    "Proceeding with the first address %s\n", print_addr(&srt_addr));
    found = 0;
  }

  freeaddrinfo(srt_addrs);

  return found;
}

#define ARG_LISTEN_PORT (argv[1])
#define ARG_SRT_HOST    (argv[2])
#define ARG_SRT_PORT    (argv[3])
int main(int argc, char **argv) {
#ifdef _WIN32
  // Windows için Winsock başlatma
  WSADATA wsaData;
  int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (wsaResult != 0) {
    fprintf(stderr, "WSAStartup failed: %d\n", wsaResult);
    return 1;
  }
#endif
  // Command line argument parsing
  if (argc >= 2 && strcmp(argv[1], "-v") == 0) {
    printf(VERSION "\n");
    exit(0);
  }
  if (argc < 4) exit_help();

  // parse optional flags after positional args
  for (int i = 4; i < argc; i++) {
    if (strcmp(argv[i], "--no-auto-reconnect") == 0) {
      flag_auto_reconnect = 0;
    } else if (strcmp(argv[i], "--auto-reconnect") == 0) {
      flag_auto_reconnect = 1;
    } else if (strcmp(argv[i], "--log-errors") == 0) {
      flag_log_errors = 1;
    } else if (strcmp(argv[i], "--reconnect-interval-ms") == 0 && i + 1 < argc) {
      flag_reconnect_interval_ms = atoi(argv[i+1]);
      i++;
    } else {
      err("Warning: unknown option %s\n", argv[i]);
    }
  }

  struct sockaddr_in listen_addr;

  int srtla_port = parse_port(ARG_LISTEN_PORT);
  if (srtla_port < 0) exit_help();
  // Try to detect if the SRT server is reachable.
  int ret = resolve_srt_addr(ARG_SRT_HOST, ARG_SRT_PORT);
  if (ret < 0) {
    exit(EXIT_FAILURE);
  }

#ifdef _WIN32
  // Windows'ta urandom yerine CryptGenRandom kullanacağız, bu değişkene ihtiyaç yok
#else
  // urandom is used to generate random ids
  urandom = fopen("/dev/urandom", "rb");
  if (urandom == NULL) {
    perror("failed to open urandom\n");
    exit(EXIT_FAILURE);
  }
#endif

#ifdef __linux__
  // We use epoll for event-driven network I/O
  socket_epoll = epoll_create(1000); // the number is ignored since Linux 2.6.8
  if (socket_epoll < 0) {
    perror("epoll_create");
    exit(1);
  }
#endif

  // Set up the listener socket for incoming SRT connections
  listen_addr.sin_family = AF_INET;
  listen_addr.sin_addr.s_addr = INADDR_ANY;
  listen_addr.sin_port = htons(srtla_port);
  srtla_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (srtla_sock < 0) {
    perror("socket creation failed");
    exit(EXIT_FAILURE);
  }

  ret = bind(srtla_sock, (const struct sockaddr *)&listen_addr, addr_len);
  if (ret < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

#ifdef __linux__
  ret = epoll_add(srtla_sock, EPOLLIN, NULL);
  if (ret != 0) {
    perror("failed to add the srtla sock to the epoll\n");
    exit(EXIT_FAILURE);
  }
#endif

  info("srtla_rec is now running\n");

  while(1) {
#ifdef __linux__
    #define MAX_EPOLL_EVENTS 10
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int eventcnt = epoll_wait(socket_epoll, events, MAX_EPOLL_EVENTS, 1000);

    time_t ts = 0;
    int ret = get_seconds(&ts);
    if (ret != 0) {
      err("Failed to get the timestamp\n");
    }
    int group_cnt;
    for (int i = 0; i < eventcnt; i++) {
      group_cnt = group_count;
      if (events[i].data.ptr == NULL) {
        handle_srtla_data(ts);
      } else {
        handle_srt_data((conn_group_t*)events[i].data.ptr);
      }
      if (group_count < group_cnt) break;
    }
    connection_cleanup(ts);
#else
    time_t ts = 0;
    int ret = get_seconds(&ts);
    if (ret != 0) {
      err("Failed to get the timestamp\n");
    }
    // Windows için select() ile hem srtla_sock hem de tüm aktif SRT socket'lerini dinle
    fd_set readfds;
    FD_ZERO(&readfds);
    int maxfd = srtla_sock;
    FD_SET(srtla_sock, &readfds);
    for (conn_group_t *g = groups; g != NULL; g = g->next) {
      if (g->srt_sock > 0) {
        FD_SET(g->srt_sock, &readfds);
        if (g->srt_sock > maxfd) maxfd = g->srt_sock;
      }
    }
    struct timeval tv = {0, 100000}; // 100ms
    int ready = select(maxfd + 1, &readfds, NULL, NULL, &tv);
    if (ready > 0) {
      if (FD_ISSET(srtla_sock, &readfds)) {
        handle_srtla_data(ts);
      }
      for (conn_group_t *g = groups; g != NULL; g = g->next) {
        if (g->srt_sock > 0 && FD_ISSET(g->srt_sock, &readfds)) {
          handle_srt_data(g);
        }
      }    }
    connection_cleanup(ts);
#endif
  } // while(1);

#ifdef _WIN32
  WSACleanup(); // Bu satır hiçbir zaman çalışmayacak çünkü while(1) döngüsünden çıkış yok, 
                // ama kodun doğruluğu için ekliyoruz
#endif
}

