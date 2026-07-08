/* odin/dns_resolver.h */

#ifndef ODIN_DNS_RESOLVER_H_
#define ODIN_DNS_RESOLVER_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "odin/event_loop.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ODIN_DNS_NAME_MAX 255

typedef struct odin_dns_resolver_t odin_dns_resolver_t;
typedef struct odin_dns_query_t odin_dns_query_t;

typedef enum odin_dns_status_t {
  ODIN_DNS_OK = 0,
  ODIN_DNS_ERROR,
} odin_dns_status_t;

typedef struct odin_dns_resolver_config_t {
  const char *servers_csv;
  int timeout_ms;
  int tries;
} odin_dns_resolver_config_t;

typedef struct odin_dns_addr_t {
  struct sockaddr_storage addr;
  socklen_t addrlen;
  int ttl;
} odin_dns_addr_t;

typedef void (*odin_dns_cb)(odin_dns_query_t *query, odin_dns_status_t status,
                            int err, const odin_dns_addr_t *addrs,
                            size_t addr_count, void *user_data);

int odin_dns_resolver_create(odin_event_loop_t *loop,
                             const odin_dns_resolver_config_t *config,
                             odin_dns_resolver_t **out);
int odin_dns_resolve_start(odin_dns_resolver_t *resolver, const char *name,
                           size_t name_len, uint16_t port, int family,
                           odin_dns_cb on_done, void *user_data,
                           odin_dns_query_t **out);
void odin_dns_query_destroy(odin_dns_query_t *query);
void odin_dns_resolver_destroy(odin_dns_resolver_t *resolver);

#ifdef __cplusplus
}
#endif

#endif /* ODIN_DNS_RESOLVER_H_ */
