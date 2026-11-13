#ifndef NB_CLEANUP_H
#define NB_CLEANUP_H

#include "nb/buf.h"
#include "nb/file.h"
#include "nb/log.h"
#include "nb/process.h"
#include "nb/map.h"
#include "nb/mem.h"
#include "nb/socket.h"
#include "nb/signal.h"
#include "nb/thread.h"
#include "nb/toml.h"
#include "nb/vec.h"

#if defined(__GNUC__) || defined(__clang__)
#define NB_CLEANUP(fn) __attribute__((__cleanup__(fn)))
#else
#define NB_CLEANUP(fn)
#endif

static inline void nb_buf_cleanup(nb_buf *buf)
{
    nb_buf_destroy(buf);
}

static inline void nb_vec_cleanup(nb_vec *vec)
{
    nb_vec_destroy(vec);
}

static inline void nb_map_cleanup(nb_map **map)
{
    if (map) {
        nb_map_free(*map);
        *map = NULL;
    }
}

static inline void nb_arena_cleanup(nb_arena **arena)
{
    if (arena) {
        nb_arena_free(*arena);
        *arena = NULL;
    }
}

static inline void nb_pool_cleanup(nb_pool **pool)
{
    if (pool) {
        nb_pool_free(*pool);
        *pool = NULL;
    }
}

static inline void nb_process_cleanup(nb_process **process)
{
    if (process) {
        nb_process_free(*process);
        *process = NULL;
    }
}

static inline void nb_log_cleanup(nb_log **log)
{
    if (log) {
        nb_log_free(*log);
        *log = NULL;
    }
}

static inline void nb_dir_cleanup(nb_dir **dir)
{
    if (dir) {
        nb_dir_close(*dir);
        *dir = NULL;
    }
}

static inline void nb_socket_cleanup(nb_socket **sock)
{
    if (sock) {
        nb_socket_free(*sock);
        *sock = NULL;
    }
}

static inline void nb_signal_listener_cleanup(nb_signal_listener **listener)
{
    (void)nb_signal_listener_stop(listener, NULL);
}

static inline void nb_toml_cleanup(nb_toml **toml)
{
    if (toml) {
        nb_toml_free(*toml);
        *toml = NULL;
    }
}

static inline void nb_thread_cleanup(nb_thread **thread)
{
    if (thread) {
        nb_thread_free(*thread);
        *thread = NULL;
    }
}

/* Release libc-owned strings (strdup/malloc) through the cleanup attribute. */
static inline void nb_charp_cleanup(char **owner)
{
    if (owner) {
        free(*owner);
        *owner = NULL;
    }
}

static inline void nb_ucharp_cleanup(unsigned char **owner)
{
    if (owner) {
        free(*owner);
        *owner = NULL;
    }
}

static inline char *nb_charp_steal(char **owner)
{
    if (!owner) {
        return NULL;
    }
    char *value = *owner;
    *owner = NULL;
    return value;
}

#endif /* NB_CLEANUP_H */
