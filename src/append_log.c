#define _GNU_SOURCE
#include "append_log.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

/*
 * On-disk / mapped record layout:
 *   uint32_t prev_off   (0 = no previous element for this list chain)
 *   uint16_t key_len
 *   uint16_t val_len
 *   key[key_len]
 *   val[val_len]
 */
typedef struct __attribute__((packed)) aol_hdr {
  uint32_t prev_off;
  uint16_t key_len;
  uint16_t val_len;
} aol_hdr_t;

static size_t record_size(uint16_t key_len, uint16_t val_len) {
  return sizeof(aol_hdr_t) + key_len + val_len;
}

int zedis_aol_init(zedis_append_log_t *log, size_t capacity, const char *path) {
  if (log == NULL || capacity < 4096) {
    return -1;
  }

  memset(log, 0, sizeof(*log));
  log->capacity = capacity;
  log->fd = -1;

  if (path != NULL && path[0] != '\0') {
    log->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (log->fd < 0) {
      return -1;
    }

    if (ftruncate(log->fd, (off_t)capacity) != 0) {
      close(log->fd);
      return -1;
    }

    log->base =
        mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, log->fd, 0);
    if (log->base == MAP_FAILED) {
      close(log->fd);
      return -1;
    }

    log->is_file = 1;
  } else {
    log->base = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (log->base == MAP_FAILED) {
      return -1;
    }
  }

  log->write_pos = 0;
  return 0;
}

void zedis_aol_destroy(zedis_append_log_t *log) {
  if (log == NULL) {
    return;
  }

  if (log->base != NULL && log->base != MAP_FAILED) {
    if (log->is_file) {
      msync(log->base, log->capacity, MS_ASYNC);
    }
    munmap(log->base, log->capacity);
  }

  if (log->fd >= 0) {
    close(log->fd);
  }

  memset(log, 0, sizeof(*log));
}

int zedis_aol_lpush(zedis_append_log_t *log, list_meta_t *meta, const char *key,
                    size_t key_len, const char *val, size_t val_len) {
  if (log == NULL || meta == NULL || key == NULL || val == NULL) {
    return -1;
  }

  if (key_len == 0 || key_len > ZEDIS_AOL_MAX_KEY || val_len == 0 ||
      val_len > ZEDIS_AOL_MAX_ELEM) {
    return -1;
  }

  size_t need = record_size((uint16_t)key_len, (uint16_t)val_len);
  if (log->write_pos + need > log->capacity) {
    return -1;
  }

  uint32_t off = (uint32_t)log->write_pos;
  aol_hdr_t *hdr = (aol_hdr_t *)(log->base + log->write_pos);
  hdr->prev_off = meta->head_off;
  hdr->key_len = (uint16_t)key_len;
  hdr->val_len = (uint16_t)val_len;

  uint8_t *payload = log->base + log->write_pos + sizeof(aol_hdr_t);
  memcpy(payload, key, key_len);
  memcpy(payload + key_len, val, val_len);

  log->write_pos += need;

  if (meta->head_off == ZEDIS_AOL_NO_PREV) {
    meta->tail_off = off;
  }
  meta->head_off = off;
  meta->count++;
  return 0;
}

static int hdr_at(const zedis_append_log_t *log, uint32_t off) {
  if (off == ZEDIS_AOL_NO_PREV || off + sizeof(aol_hdr_t) > log->capacity) {
    return 0;
  }
  return 1;
}

static const aol_hdr_t *hdr_ptr(const zedis_append_log_t *log, uint32_t off) {
  return (const aol_hdr_t *)(log->base + off);
}

static const char *val_at(const aol_hdr_t *hdr) {
  return (const char *)hdr + sizeof(aol_hdr_t) + hdr->key_len;
}

int zedis_aol_lrange(const zedis_append_log_t *log, const list_meta_t *meta,
                     int64_t start, int64_t stop, char *reply, size_t reply_cap,
                     int *reply_len) {
  if (log == NULL || meta == NULL || reply == NULL || reply_len == NULL) {
    return -1;
  }

  if (meta->count == 0 || meta->head_off == ZEDIS_AOL_NO_PREV) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }

  /* Collect offsets from head (newest) backward via prev_off chain. */
  uint32_t offsets[1024];
  size_t chain_len = 0;
  uint32_t cur = meta->head_off;

  while (cur != ZEDIS_AOL_NO_PREV &&
         chain_len < sizeof(offsets) / sizeof(offsets[0])) {
    const aol_hdr_t *hdr = hdr_ptr(log, cur);
    if (!hdr_at(log, cur)) {
      break;
    }
    offsets[chain_len++] = cur;
    cur = hdr->prev_off;
  }

  if (start < 0) {
    start = 0;
  }
  if (stop < 0 || stop >= (int64_t)chain_len) {
    stop = (int64_t)chain_len - 1;
  }
  if (start > stop) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }

  int count = (int)(stop - start + 1);
  int n = snprintf(reply, reply_cap, "*%d\r\n", count);
  if (n <= 0 || (size_t)n >= reply_cap) {
    return -1;
  }
  *reply_len = n;

  for (int64_t i = start; i <= stop; i++) {
    uint32_t off = offsets[(size_t)i];
    if (!hdr_at(log, off)) {
      return -1;
    }
    const aol_hdr_t *hdr = hdr_ptr(log, off);
    const char *val = val_at(hdr);
    int m = snprintf(reply + *reply_len, reply_cap - (size_t)*reply_len,
                     "$%u\r\n%.*s\r\n", hdr->val_len, (int)hdr->val_len, val);
    if (m <= 0 || (size_t)*reply_len + (size_t)m >= reply_cap) {
      return -1;
    }
    *reply_len += m;
  }

  return 0;
}

uint32_t zedis_aol_llen(const list_meta_t *meta) {
  if (meta == NULL) {
    return 0;
  }
  return meta->count;
}
