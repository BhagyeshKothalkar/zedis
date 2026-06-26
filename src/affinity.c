#define _GNU_SOURCE
#include "affinity.h"

#include <sched.h>

int zedis_pin_to_core(int core) {
  if (core < 0) {
    return 0;
  }

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET((unsigned)core, &cpuset);

  if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
    return -1;
  }

  return 0;
}
