#ifndef ZEDIS_APPEND_LOG_H
#define ZEDIS_APPEND_LOG_H

#include <stddef.h>
#include <stdint.h>

#define ZEDIS_AOL_MAX_ELEM 512
#define ZEDIS_AOL_MAX_KEY 256
#define ZEDIS_AOL_DEFAULT_SIZE (16ULL * 1024ULL * 1024ULL)

typedef struct list_meta {
  uint32_t head_off;
  uint32_t tail_off;
  uint32_t count;
} list_meta_t;

#define ZEDIS_AOL_NO_PREV UINT32_MAX

typedef struct zedis_append_log {
  uint8_t *base;
  size_t capacity;
  size_t write_pos;
  int fd;
  int is_file;
} zedis_append_log_t;

int zedis_aol_init(zedis_append_log_t *log, size_t capacity, const char *path);
void zedis_aol_destroy(zedis_append_log_t *log);

int zedis_aol_lpush(zedis_append_log_t *log, list_meta_t *meta, const char *key,
                    size_t key_len, const char *val, size_t val_len);
int zedis_aol_lrange(const zedis_append_log_t *log, const list_meta_t *meta,
                     int64_t start, int64_t stop, char *reply, size_t reply_cap,
                     int *reply_len);
uint32_t zedis_aol_llen(const list_meta_t *meta);

#endif /* ZEDIS_APPEND_LOG_H */
