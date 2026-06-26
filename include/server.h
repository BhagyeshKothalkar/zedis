#ifndef ZEDIS_SERVER_H
#define ZEDIS_SERVER_H

/*
 * server.h — zedis server core
 *
 * Connection read path (changed)
 * --------------------------------
 * Previously each conn_t held a flat read buffer and called resp_parse() on
 * it directly, requiring the connection layer to manually slide unconsumed
 * bytes around after each read(2).
 *
 * Now each conn_t embeds a resp_parser_t.  The readable callback does:
 *
 *   1. read(fd, resp_parser_write_ptr(&conn->parser),
 *              resp_parser_space(&conn->parser))
 *   2. resp_parser_commit(&conn->parser, n_read)
 *   3. loop: resp_parser_next() → server_handle_command() until NEED_MORE
 *
 * This handles all of:
 *   • Partial messages   — NEED_MORE, data stays in the parser buffer.
 *   • Pipelined messages — the loop drains every complete command in one
 *                          pass, no second read(2) needed.
 *   • Mixed partial+full — handled naturally by the combination above.
 *
 * server_handle_command() is kept for single-command dispatch.
 * server_dispatch_commands() is the new loop wrapper called from conn.c.
 */

#include "append_log.h"
#include "arena.h"
#include "conn.h"
#include "event_loop.h"
#include "hash_table.h"
#include "key_registry.h"
#include "order_book.h"
#include "ring_buffer.h"
#include "slab.h"
#include "zedis.h"

#include <stdbool.h>

struct zedis_server {
    zedis_config_t  config;
    event_loop_t   *loop;
    int             listen_fd;
    bool            running;

    zedis_arena_t        arena;
    zedis_slab_t         slab;
    zedis_hash_table_t   kv;
    zedis_order_book_t   book;
    zedis_ring_buffer_t  ring;
    zedis_append_log_t   aol;
    zedis_zset_registry_t zsets;
    zedis_list_registry_t lists;

    conn_t *conns_head;
    size_t  conn_count;
};

/* -------------------------------------------------------------------------
 * Connection list management
 * ---------------------------------------------------------------------- */

void server_link_conn  (zedis_server_t *server, conn_t *conn);
void server_unlink_conn(zedis_server_t *server, conn_t *conn);

/* -------------------------------------------------------------------------
 * Pub/sub broadcast
 * ---------------------------------------------------------------------- */

void server_broadcast_channel(zedis_server_t *server, const char *channel,
                               const char *payload, size_t payload_len);

/* -------------------------------------------------------------------------
 * Command dispatch
 * ---------------------------------------------------------------------- */

/*
 * Dispatch a single already-parsed RESP command.
 *
 * Returns 0  — command handled, connection still alive.
 * Returns 1  — connection was closed (e.g. QUIT command).
 */
int server_handle_command(conn_t *conn, resp_value_t *cmd);

/*
 * Drain the parser embedded in *conn and dispatch every complete command.
 *
 * Called by conn_on_readable() after each successful read(2):
 *
 *   resp_parser_commit(&conn->parser, n);
 *   server_dispatch_commands(conn);           // handles 0..N commands
 *
 * Returns 0  — still alive after draining.
 * Returns 1  — connection was closed during dispatch (e.g. QUIT).
 *
 * The function stops dispatching as soon as the connection is destroyed;
 * callers must not touch *conn after a return value of 1.
 */
int server_dispatch_commands(conn_t *conn);

/* -------------------------------------------------------------------------
 * Accept handler (registered with the event loop)
 * ---------------------------------------------------------------------- */

void server_on_accept(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);

#endif /* ZEDIS_SERVER_H */