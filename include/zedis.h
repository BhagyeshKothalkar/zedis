#ifndef ZEDIS_H
#define ZEDIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZEDIS_DEFAULT_PORT 6379
#define ZEDIS_MAX_EVENTS 1024
#define ZEDIS_MAX_FDS 4096
#define ZEDIS_DEFAULT_MAX_KEYS 8192
#define ZEDIS_DEFAULT_MAX_CONNS 1024
#define ZEDIS_DEFAULT_RING_CAP 4096
#define ZEDIS_DEFAULT_BOOK_MIN 0
#define ZEDIS_DEFAULT_BOOK_MAX 99999
#define ZEDIS_CONN_READ_SIZE 4096
#define ZEDIS_CONN_WRITE_SIZE 4096
#define ZEDIS_DEFAULT_MAX_ZSETS 256
#define ZEDIS_DEFAULT_MAX_LISTS 256
#define ZEDIS_DEFAULT_ZSET_MEMBERS 2048
#define ZEDIS_DEFAULT_AOL_SIZE (16ULL * 1024ULL * 1024ULL)

typedef struct zedis_server zedis_server_t;

typedef struct zedis_config {
  uint16_t port;
  int cpu_core; /* -1 = no pinning */
  size_t arena_size;
  size_t max_keys;
  size_t max_connections;
  size_t ring_capacity;
  size_t max_zsets;
  size_t max_lists;
  size_t zset_members;
  size_t aol_size;
  const char *aol_path;
  int book_price_min;
  int book_price_max;
  bool busy_poll;
} zedis_config_t;

zedis_server_t *zedis_create(const zedis_config_t *config);
void zedis_destroy(zedis_server_t *server);
int zedis_run(zedis_server_t *server);

#endif /* ZEDIS_H */
