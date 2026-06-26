#include "arena.h"
#include "zedis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [options]\n"
          "  --port PORT           TCP port (default %u)\n"
          "  --core CPU            Pin main thread to CPU core (-1 = no pin)\n"
          "  --arena-size BYTES    Memory arena size (default 64MiB)\n"
          "  --max-keys N          Hash table capacity (default %zu)\n"
          "  --max-conns N         Max connections (default %zu)\n"
          "  --ring-cap N          Pub/sub ring capacity (default %zu)\n"
          "  --max-zsets N         Max sorted sets (default %zu)\n"
          "  --zset-members N      Max members per sorted set (default %zu)\n"
          "  --max-lists N         Max lists (default %zu)\n"
          "  --aol-size BYTES      Append log size (default %zu)\n"
          "  --aol-path PATH       Append log file (default anonymous mmap)\n"
          "  --book-min PRICE      Order book min price (default %d)\n"
          "  --book-max PRICE      Order book max price (default %d)\n"
          "  --no-busy-poll        Use blocking epoll (dev/debug only)\n",
          prog, ZEDIS_DEFAULT_PORT, (size_t)ZEDIS_DEFAULT_MAX_KEYS,
          (size_t)ZEDIS_DEFAULT_MAX_CONNS, (size_t)ZEDIS_DEFAULT_RING_CAP,
          (size_t)ZEDIS_DEFAULT_MAX_ZSETS, (size_t)ZEDIS_DEFAULT_ZSET_MEMBERS,
          (size_t)ZEDIS_DEFAULT_MAX_LISTS,
          (size_t)ZEDIS_DEFAULT_AOL_SIZE, ZEDIS_DEFAULT_BOOK_MIN,
          ZEDIS_DEFAULT_BOOK_MAX);
}

static zedis_config_t default_config(void) {
  zedis_config_t cfg = {
      .port = ZEDIS_DEFAULT_PORT,
      .cpu_core = -1,
      .arena_size = ZEDIS_DEFAULT_ARENA_SIZE,
      .max_keys = ZEDIS_DEFAULT_MAX_KEYS,
      .max_connections = ZEDIS_DEFAULT_MAX_CONNS,
      .ring_capacity = ZEDIS_DEFAULT_RING_CAP,
      .max_zsets = ZEDIS_DEFAULT_MAX_ZSETS,
      .max_lists = ZEDIS_DEFAULT_MAX_LISTS,
      .zset_members = ZEDIS_DEFAULT_ZSET_MEMBERS,
      .aol_size = ZEDIS_DEFAULT_AOL_SIZE,
      .aol_path = NULL,
      .book_price_min = ZEDIS_DEFAULT_BOOK_MIN,
      .book_price_max = ZEDIS_DEFAULT_BOOK_MAX,
      .busy_poll = true,
  };
  return cfg;
}

int main(int argc, char **argv) {
  zedis_config_t config = default_config();

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.port = (uint16_t)atoi(argv[++i]);
      continue;
    }

    if (strcmp(argv[i], "--core") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.cpu_core = atoi(argv[++i]);
      continue;
    }

    if (strcmp(argv[i], "--arena-size") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.arena_size = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--max-keys") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.max_keys = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--max-conns") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.max_connections = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--ring-cap") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.ring_capacity = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--max-zsets") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.max_zsets = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--zset-members") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.zset_members = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--max-lists") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.max_lists = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--aol-size") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.aol_size = (size_t)strtoull(argv[++i], NULL, 10);
      continue;
    }

    if (strcmp(argv[i], "--aol-path") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.aol_path = argv[++i];
      continue;
    }

    if (strcmp(argv[i], "--book-min") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.book_price_min = atoi(argv[++i]);
      continue;
    }

    if (strcmp(argv[i], "--book-max") == 0) {
      if (i + 1 >= argc) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
      }
      config.book_price_max = atoi(argv[++i]);
      continue;
    }

    if (strcmp(argv[i], "--no-busy-poll") == 0) {
      config.busy_poll = false;
      continue;
    }

    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  zedis_server_t *server = zedis_create(&config);
  if (server == NULL) {
    fprintf(stderr, "zedis: failed to start server on port %u\n", config.port);
    return EXIT_FAILURE;
  }

  fprintf(stderr, "zedis listening on port %u (busy-poll=%s, arena=%zu bytes",
          config.port, config.busy_poll ? "on" : "off", config.arena_size);
  if (config.cpu_core >= 0) {
    fprintf(stderr, ", core=%d", config.cpu_core);
  }
  fprintf(stderr, ")\n");

  int rc = zedis_run(server);
  zedis_destroy(server);
  return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
