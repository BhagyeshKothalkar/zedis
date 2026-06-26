#include "event_loop.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "zedis.h"

struct event_loop {
  int epfd;
  int stop;
  int max_fds;
  struct epoll_event events[ZEDIS_MAX_EVENTS];
  struct {
    event_handler_fn handler;
    void *userdata;
  } *handlers;
};

event_loop_t *event_loop_create(zedis_arena_t *arena, int max_fds) {
  if (arena == NULL || max_fds <= 0) {
    return NULL;
  }

  event_loop_t *loop = zedis_arena_calloc(arena, 1, sizeof(*loop), 64);
  if (loop == NULL) {
    return NULL;
  }

  loop->max_fds = max_fds;
  loop->handlers =
      zedis_arena_calloc(arena, (size_t)max_fds, sizeof(*loop->handlers), 64);
  if (loop->handlers == NULL) {
    return NULL;
  }

  loop->epfd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->epfd < 0) {
    return NULL;
  }

  return loop;
}

void event_loop_destroy(event_loop_t *loop) {
  if (loop == NULL) {
    return;
  }

  if (loop->epfd >= 0) {
    close(loop->epfd);
  }
}

int event_loop_add(event_loop_t *loop, int fd, uint32_t events,
                   event_handler_fn handler, void *userdata) {
  if (fd < 0 || fd >= loop->max_fds) {
    return -1;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    return -1;
  }

  loop->handlers[fd].handler = handler;
  loop->handlers[fd].userdata = userdata;
  return 0;
}

int event_loop_mod(event_loop_t *loop, int fd, uint32_t events,
                   event_handler_fn handler, void *userdata) {
  if (fd < 0 || fd >= loop->max_fds) {
    return -1;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;

  if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev) != 0) {
    return -1;
  }

  loop->handlers[fd].handler = handler;
  loop->handlers[fd].userdata = userdata;
  return 0;
}

int event_loop_del(event_loop_t *loop, int fd) {
  if (epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL) != 0) {
    return -1;
  }

  if (fd >= 0 && fd < loop->max_fds) {
    loop->handlers[fd].handler = NULL;
    loop->handlers[fd].userdata = NULL;
  }

  return 0;
}

int event_loop_run_once(event_loop_t *loop, int block) {
  int timeout = block ? -1 : 0;
  int n = epoll_wait(loop->epfd, loop->events, ZEDIS_MAX_EVENTS, timeout);
  if (n < 0) {
    return errno == EINTR ? 0 : -1;
  }

  for (int i = 0; i < n; i++) {
    int fd = loop->events[i].data.fd;
    if (fd < 0 || fd >= loop->max_fds) {
      continue;
    }

    event_handler_fn handler = loop->handlers[fd].handler;
    if (handler != NULL) {
      handler(loop, fd, loop->events[i].events, loop->handlers[fd].userdata);
    }
  }

  return n;
}

void event_loop_stop(event_loop_t *loop) { loop->stop = 1; }
