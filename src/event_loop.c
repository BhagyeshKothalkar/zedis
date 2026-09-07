#define _POSIX_C_SOURCE 200809L

#include "event_loop.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "zedis.h"

static volatile sig_atomic_t signal_received;
static struct sigaction previous_sigterm;
static struct sigaction previous_sigint;
static int signal_handlers_installed;

static void handle_shutdown_signal(int signum) { signal_received = signum; }

int event_loop_install_signal_handlers(void) {
  struct sigaction action;

  memset(&action, 0, sizeof(action));
  action.sa_handler = handle_shutdown_signal;
  sigemptyset(&action.sa_mask);

  if (sigaction(SIGTERM, &action, &previous_sigterm) != 0) {
    return -1;
  }
  if (sigaction(SIGINT, &action, &previous_sigint) != 0) {
    sigaction(SIGTERM, &previous_sigterm, NULL);
    return -1;
  }

  signal_received = 0;
  signal_handlers_installed = 1;
  return 0;
}

void event_loop_restore_signal_handlers(void) {
  if (!signal_handlers_installed) {
    return;
  }

  sigaction(SIGTERM, &previous_sigterm, NULL);
  sigaction(SIGINT, &previous_sigint, NULL);
  signal_handlers_installed = 0;
}

int event_loop_signal_received(void) { return signal_received != 0; }

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
  if (loop == NULL || loop->stop || signal_received != 0) {
    if (loop != NULL) {
      loop->stop = 1;
    }
    return -1;
  }

  int timeout = block ? -1 : 0;
  int n = epoll_wait(loop->epfd, loop->events, ZEDIS_MAX_EVENTS, timeout);
  if (n < 0) {
    if (signal_received != 0) {
      loop->stop = 1;
      return -1;
    }
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

void event_loop_stop(event_loop_t *loop) {
  if (loop != NULL) {
    loop->stop = 1;
  }
}
