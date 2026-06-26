#ifndef ZEDIS_EVENT_LOOP_H
#define ZEDIS_EVENT_LOOP_H

#include "arena.h"

#include <stddef.h>
#include <stdint.h>

typedef struct event_loop event_loop_t;

typedef void (*event_handler_fn)(event_loop_t *loop, int fd, uint32_t events,
                                 void *userdata);

event_loop_t *event_loop_create(zedis_arena_t *arena, int max_fds);
void event_loop_destroy(event_loop_t *loop);

int event_loop_add(event_loop_t *loop, int fd, uint32_t events,
                   event_handler_fn handler, void *userdata);
int event_loop_mod(event_loop_t *loop, int fd, uint32_t events,
                   event_handler_fn handler, void *userdata);
int event_loop_del(event_loop_t *loop, int fd);

int event_loop_run_once(event_loop_t *loop, int block);
void event_loop_stop(event_loop_t *loop);

#endif /* ZEDIS_EVENT_LOOP_H */
