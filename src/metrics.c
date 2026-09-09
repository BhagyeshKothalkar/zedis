#define _POSIX_C_SOURCE 200809L

#include "metrics.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "server.h"

#define ZEDIS_METRICS_FILE "zedis.prom"

static const char *const command_names[] = {
    "PING", "ECHO", "SET", "GET", "BID", "ASK", "BOOK", "DEL", "ZADD",
    "ZSCORE", "ZRANGE", "LPUSH", "LRANGE", "LLEN", "PUBLISH", "SUBSCRIBE",
    "QUIT"};

typedef struct zedis_metrics {
  char *collector_dir;
  unsigned long long interval_ms;
  uint64_t last_write_ms;
  uint64_t command_counts[sizeof(command_names) / sizeof(command_names[0])];
} zedis_metrics_t;

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)now.tv_nsec / 1000000ULL;
}

static int command_index(const char *name, size_t name_len) {
  for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); i++) {
    if (strlen(command_names[i]) == name_len &&
        strncmp(command_names[i], name, name_len) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static int write_snapshot(const zedis_server_t *server,
                          const zedis_metrics_t *metrics) {
  size_t dir_len = strlen(metrics->collector_dir);
  size_t temp_len = dir_len + 32;
  size_t final_len = dir_len + 1 + sizeof(ZEDIS_METRICS_FILE);
  char *temp_path = malloc(temp_len);
  char *final_path = malloc(final_len);
  if (temp_path == NULL || final_path == NULL) {
    free(temp_path);
    free(final_path);
    return -1;
  }

  int temp_written = snprintf(temp_path, temp_len, "%s/.zedis.prom.XXXXXX",
                              metrics->collector_dir);
  int final_written = snprintf(final_path, final_len, "%s/%s",
                               metrics->collector_dir, ZEDIS_METRICS_FILE);
  if (temp_written < 0 || final_written < 0 || (size_t)temp_written >= temp_len ||
      (size_t)final_written >= final_len) {
    free(temp_path);
    free(final_path);
    return -1;
  }

  int fd = mkstemp(temp_path);
  if (fd < 0) {
    free(temp_path);
    free(final_path);
    return -1;
  }
  if (fchmod(fd, 0644) != 0) {
    close(fd);
    unlink(temp_path);
    free(temp_path);
    free(final_path);
    return -1;
  }
  FILE *file = fdopen(fd, "w");
  if (file == NULL) {
    close(fd);
    unlink(temp_path);
    free(temp_path);
    free(final_path);
    return -1;
  }

  int failed = 0;
#define METRIC_LINE(...) \
  do { \
    if (fprintf(file, __VA_ARGS__) < 0) failed = 1; \
  } while (0)
  METRIC_LINE("# HELP zedis_up Whether the Zedis metrics writer is active.\n");
  METRIC_LINE("# TYPE zedis_up gauge\nzedis_up 1\n");
  METRIC_LINE("# HELP zedis_connections Current client connections.\n");
  METRIC_LINE("# TYPE zedis_connections gauge\nzedis_connections %zu\n",
              server->conn_count);
  METRIC_LINE("# HELP zedis_keys Current number of string keys.\n");
  METRIC_LINE("# TYPE zedis_keys gauge\nzedis_keys %zu\n", server->kv.count);
  METRIC_LINE("# HELP zedis_zsets Current number of sorted sets.\n");
  METRIC_LINE("# TYPE zedis_zsets gauge\nzedis_zsets %u\n", server->zsets.used);
  METRIC_LINE("# HELP zedis_lists Current number of lists.\n");
  METRIC_LINE("# TYPE zedis_lists gauge\nzedis_lists %u\n", server->lists.used);
  METRIC_LINE("# HELP zedis_arena_bytes_used Bytes used by the memory arena.\n");
  METRIC_LINE("# TYPE zedis_arena_bytes_used gauge\nzedis_arena_bytes_used %zu\n",
              server->arena.offset);
  METRIC_LINE("# HELP zedis_arena_bytes_capacity Total arena capacity in bytes.\n");
  METRIC_LINE("# TYPE zedis_arena_bytes_capacity gauge\nzedis_arena_bytes_capacity %zu\n",
              server->arena.size);
  METRIC_LINE("# HELP zedis_commands_total Commands handled by command name.\n");
  METRIC_LINE("# TYPE zedis_commands_total counter\n");
  for (size_t i = 0; i < sizeof(command_names) / sizeof(command_names[0]); i++) {
    METRIC_LINE("zedis_commands_total{command=\"%s\"} %" PRIu64 "\n",
                command_names[i], metrics->command_counts[i]);
  }
#undef METRIC_LINE

  if (fflush(file) != 0 || fsync(fd) != 0) failed = 1;
  if (fclose(file) != 0) failed = 1;
  if (!failed && rename(temp_path, final_path) != 0) failed = 1;
  if (failed) unlink(temp_path);

  free(temp_path);
  free(final_path);
  return failed ? -1 : 0;
}

int zedis_metrics_init(zedis_server_t *server, const char *collector_dir,
                       unsigned long long interval_ms) {
  if (server == NULL || collector_dir == NULL || collector_dir[0] == '\0') {
    return -1;
  }
  zedis_metrics_t *metrics = calloc(1, sizeof(*metrics));
  if (metrics == NULL) return -1;
  metrics->collector_dir = strdup(collector_dir);
  if (metrics->collector_dir == NULL) {
    free(metrics);
    return -1;
  }
  metrics->interval_ms = interval_ms == 0 ? 1000ULL : interval_ms;
  server->metrics = metrics;
  return 0;
}

void zedis_metrics_destroy(zedis_server_t *server) {
  if (server == NULL || server->metrics == NULL) return;
  free(server->metrics->collector_dir);
  free(server->metrics);
  server->metrics = NULL;
}

void zedis_metrics_record_command(zedis_server_t *server, const char *name,
                                  size_t name_len) {
  if (server == NULL || server->metrics == NULL || name == NULL) return;
  int index = command_index(name, name_len);
  if (index >= 0) server->metrics->command_counts[(size_t)index]++;
}

void zedis_metrics_maybe_write(zedis_server_t *server) {
  if (server == NULL || server->metrics == NULL) return;
  uint64_t now = monotonic_ms();
  if (server->metrics->last_write_ms != 0 &&
      now - server->metrics->last_write_ms < server->metrics->interval_ms) {
    return;
  }
  (void)write_snapshot(server, server->metrics);
  server->metrics->last_write_ms = now;
}
