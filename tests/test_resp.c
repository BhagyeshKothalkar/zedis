#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resp.h"

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg)                                     \
  do {                                                        \
    tests_run++;                                              \
    if (!(cond)) {                                            \
      fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
      tests_failed++;                                         \
    }                                                         \
  } while (0)

static void test_formatters(void) {
  char buf[256];
  int n;

  /* Simple string */
  n = resp_format_simple(buf, sizeof(buf), "OK");
  ASSERT(n == 5, "format simple len");
  ASSERT(strcmp(buf, "+OK\r\n") == 0, "format simple content");

  /* Error string */
  n = resp_format_error(buf, sizeof(buf), "ERR failed");
  ASSERT(n == 13, "format error len");
  ASSERT(strcmp(buf, "-ERR failed\r\n") == 0, "format error content");

  /* Integer */
  n = resp_format_integer(buf, sizeof(buf), 98765);
  ASSERT(n == 8, "format integer len");
  ASSERT(strcmp(buf, ":98765\r\n") == 0, "format integer content");

  /* Bulk string */
  n = resp_format_bulk(buf, sizeof(buf), "hello", 5);
  ASSERT(n == 11, "format bulk len");
  ASSERT(strcmp(buf, "$5\r\nhello\r\n") == 0, "format bulk content");

  /* Null bulk string */
  n = resp_format_null(buf, sizeof(buf));
  ASSERT(n == 5, "format null len");
  ASSERT(strcmp(buf, "$-1\r\n") == 0, "format null content");
}

static void test_parser_basics(void) {
  resp_parser_t parser;
  ASSERT(resp_parser_init(&parser, NULL) == 0, "parser init");

  resp_value_t val;
  memset(&val, 0, sizeof(val));

  /* Simple String */
  ASSERT(resp_parser_feed(&parser, "+PING\r\n", 7) == 0, "feed simple");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse simple");
  ASSERT(val.type == RESP_TYPE_SIMPLE, "type simple");
  ASSERT(val.bulk_len == 4 && memcmp(val.bulk, "PING", 4) == 0,
         "simple content");
  resp_value_release(&parser, &val);

  /* Error */
  ASSERT(resp_parser_feed(&parser, "-ERR custom\r\n", 13) == 0, "feed error");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse error");
  ASSERT(val.type == RESP_TYPE_ERROR, "type error");
  ASSERT(val.bulk_len == 10 && memcmp(val.bulk, "ERR custom", 10) == 0,
         "error content");
  resp_value_release(&parser, &val);

  /* Integer */
  ASSERT(resp_parser_feed(&parser, ":123456\r\n", 9) == 0, "feed integer");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse integer");
  ASSERT(val.type == RESP_TYPE_INTEGER, "type integer");
  ASSERT(val.integer == 123456, "integer value");
  resp_value_release(&parser, &val);

  /* Bulk String */
  ASSERT(resp_parser_feed(&parser, "$4\r\ntest\r\n", 10) == 0, "feed bulk");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse bulk");
  ASSERT(val.type == RESP_TYPE_BULK, "type bulk");
  ASSERT(val.bulk_len == 4 && memcmp(val.bulk, "test", 4) == 0, "bulk content");
  resp_value_release(&parser, &val);

  /* Null Bulk String */
  ASSERT(resp_parser_feed(&parser, "$-1\r\n", 5) == 0, "feed null");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse null");
  ASSERT(val.type == RESP_TYPE_NULL, "type null");
  resp_value_release(&parser, &val);

  resp_parser_destroy(&parser);
}

static void test_parser_array(void) {
  resp_parser_t parser;
  ASSERT(resp_parser_init(&parser, NULL) == 0, "parser init");

  resp_value_t val;
  memset(&val, 0, sizeof(val));

  /* Array: *2\r\n$3\r\nGET\r\n$4\r\nkeys\r\n */
  const char *data = "*2\r\n$3\r\nGET\r\n$4\r\nkeys\r\n";
  ASSERT(resp_parser_feed(&parser, data, strlen(data)) == 0, "feed array");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse array");
  ASSERT(val.type == RESP_TYPE_ARRAY, "type array");
  ASSERT(val.array_count == 2, "array count");

  resp_node_t *n1 = val.array_head;
  ASSERT(n1 != NULL, "node 1 exists");
  ASSERT(n1->type == RESP_TYPE_BULK, "node 1 type");
  ASSERT(n1->bulk_len == 3 && memcmp(n1->bulk, "GET", 3) == 0,
         "node 1 content");

  resp_node_t *n2 = n1->next;
  ASSERT(n2 != NULL, "node 2 exists");
  ASSERT(n2->type == RESP_TYPE_BULK, "node 2 type");
  ASSERT(n2->bulk_len == 4 && memcmp(n2->bulk, "keys", 4) == 0,
         "node 2 content");

  resp_value_release(&parser, &val);
  resp_parser_destroy(&parser);
}

static void test_parser_fragmented(void) {
  resp_parser_t parser;
  ASSERT(resp_parser_init(&parser, NULL) == 0, "parser init");

  resp_value_t val;
  memset(&val, 0, sizeof(val));

  /* Fragmented simple string feed */
  ASSERT(resp_parser_feed(&parser, "+PI", 3) == 0, "feed frag 1");
  ASSERT(resp_parser_next(&parser, &val) == RESP_NEED_MORE,
         "parse frag 1 needs more");

  ASSERT(resp_parser_feed(&parser, "NG\r", 3) == 0, "feed frag 2");
  ASSERT(resp_parser_next(&parser, &val) == RESP_NEED_MORE,
         "parse frag 2 needs more");

  ASSERT(resp_parser_feed(&parser, "\n", 1) == 0, "feed frag 3");
  ASSERT(resp_parser_next(&parser, &val) == RESP_OK, "parse frag 3 ok");
  ASSERT(val.type == RESP_TYPE_SIMPLE, "type fragmented simple");
  ASSERT(val.bulk_len == 4 && memcmp(val.bulk, "PING", 4) == 0,
         "fragmented simple content");

  resp_value_release(&parser, &val);
  resp_parser_destroy(&parser);
}

int main(void) {
  test_formatters();
  test_parser_basics();
  test_parser_array();
  test_parser_fragmented();

  printf("Ran %d RESP tests, %d failed\n", tests_run, tests_failed);
  return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
