#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "resp.h"

static int append_str(char *dst, size_t cap, size_t *used, const char *s,
                      size_t len) {
  if (*used + len >= cap) return -1;
  memcpy(dst + *used, s, len);
  *used += len;
  return 0;
}
int resp_format_simple(char *dst, size_t cap, const char *str) {
  size_t used = 0, len = strlen(str);
  if (append_str(dst, cap, &used, "+", 1) != 0) return -1;
  if (append_str(dst, cap, &used, str, len) != 0) return -1;
  if (append_str(dst, cap, &used, "\r\n", 2) != 0) return -1;
  dst[used] = '\0';
  return (int)used;
}
int resp_format_error(char *dst, size_t cap, const char *msg) {
  size_t used = 0, len = strlen(msg);
  if (append_str(dst, cap, &used, "-", 1) != 0) return -1;
  if (append_str(dst, cap, &used, msg, len) != 0) return -1;
  if (append_str(dst, cap, &used, "\r\n", 2) != 0) return -1;
  dst[used] = '\0';
  return (int)used;
}
int resp_format_integer(char *dst, size_t cap, int64_t value) {
  int n = snprintf(dst, cap, ":%" PRId64 "\r\n", value);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}
int resp_format_bulk(char *dst, size_t cap, const char *data, size_t len) {
  int n = snprintf(dst, cap, "$%zu\r\n", len);
  if (n <= 0 || (size_t)n >= cap) return -1;
  size_t used = (size_t)n;
  if (used + len + 3 > cap) return -1;
  memcpy(dst + used, data, len);
  used += len;
  memcpy(dst + used, "\r\n", 2);
  used += 2;
  dst[used] = '\0';
  return (int)used;
}
int resp_format_null(char *dst, size_t cap) {
  static const char null_bulk[] = "$-1\r\n";
  size_t len = sizeof(null_bulk) - 1;
  if (len >= cap) return -1;
  memcpy(dst, null_bulk, len);
  dst[len] = '\0';
  return (int)len;
}