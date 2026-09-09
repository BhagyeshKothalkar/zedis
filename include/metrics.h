#ifndef ZEDIS_METRICS_H
#define ZEDIS_METRICS_H

#include <stddef.h>

typedef struct zedis_server zedis_server_t;

int zedis_metrics_init(zedis_server_t *server, const char *collector_dir,
                       unsigned long long interval_ms);
void zedis_metrics_destroy(zedis_server_t *server);
void zedis_metrics_record_command(zedis_server_t *server, const char *name,
                                  size_t name_len);
void zedis_metrics_maybe_write(zedis_server_t *server);

#endif /* ZEDIS_METRICS_H */
