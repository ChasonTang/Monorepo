/* odin/server_runtime.c -- RFC-021 per-listener server runtime. */

#include "odin/server_runtime.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "odin/accept_loop.h"

#if defined(ODIN_SERVER_RUNTIME_TESTING)
#include "odin/server_runtime_internal_test.h"
#endif

typedef struct odin_server_runtime_entry_t odin_server_runtime_entry_t;

struct odin_server_runtime_entry_t {
  odin_server_session_t *ss;
  odin_server_runtime_entry_t *prev;
  odin_server_runtime_entry_t *next;
};

struct odin_server_runtime_t {
  odin_event_loop_t *loop;
  int listen_fd;
  odin_accept_loop_t *al;
  odin_server_runtime_entry_t *head;
  odin_server_runtime_error_cb on_runtime_error;
  void *user_data;
  odin_server_session_dial_filter_cb dial_filter;
  void *dial_filter_ud;
  int terminal;
  int in_callback;
  int destroy_pending;
#if defined(ODIN_SERVER_RUNTIME_TESTING)
  int fail_next_session_create_armed;
  int fail_next_entry_alloc_armed;
#endif
};

static void runtime_on_accept(odin_accept_loop_t *al, int conn_fd,
                              void *user_data);
static void runtime_on_accept_error(odin_accept_loop_t *al, int err,
                                    void *user_data);
static void runtime_on_close(odin_server_session_t *ss, int err,
                             void *user_data);
static void finish_destroy(odin_server_runtime_t *rt);

int odin_server_runtime_create(odin_event_loop_t *loop, int listen_fd,
                               odin_server_runtime_error_cb on_runtime_error,
                               void *user_data,
                               odin_server_runtime_t **out) {
  odin_server_runtime_t *rt =
      (odin_server_runtime_t *)calloc(1, sizeof(*rt));
  if (rt == NULL) {
    errno = ENOMEM;
    return -1;
  }
  rt->loop = loop;
  rt->listen_fd = listen_fd;
  rt->al = NULL;
  rt->head = NULL;
  rt->on_runtime_error = on_runtime_error;
  rt->user_data = user_data;
  rt->dial_filter = NULL;
  rt->dial_filter_ud = NULL;
  rt->terminal = 0;
  rt->in_callback = 0;
  rt->destroy_pending = 0;
  if (odin_accept_loop_create(loop, listen_fd, runtime_on_accept,
                              runtime_on_accept_error, rt, &rt->al) != 0) {
    const int saved = errno;
    free(rt);
    errno = saved;
    return -1;
  }
  *out = rt;
  return 0;
}

void odin_server_runtime_set_dial_filter(
    odin_server_runtime_t *rt, odin_server_session_dial_filter_cb cb,
    void *user_data) {
  if (rt == NULL) {
    return;
  }
  rt->dial_filter = cb;
  rt->dial_filter_ud = user_data;
}

void odin_server_runtime_destroy(odin_server_runtime_t *rt) {
  if (rt == NULL) {
    return;
  }
  if (rt->in_callback) {
    rt->destroy_pending = 1;
    return;
  }
  finish_destroy(rt);
}

static void runtime_on_accept(odin_accept_loop_t *al, int conn_fd,
                              void *user_data) {
  (void)al;
  odin_server_runtime_t *rt = (odin_server_runtime_t *)user_data;
  odin_server_session_t *ss = NULL;
#if defined(ODIN_SERVER_RUNTIME_TESTING)
  if (rt->fail_next_session_create_armed) {
    rt->fail_next_session_create_armed = 0;
    close(conn_fd);
    return;
  }
#endif
  if (odin_server_session_create(rt->loop, conn_fd, runtime_on_close, rt,
                                 &ss) != 0) {
    close(conn_fd);
    return;
  }
  odin_server_runtime_entry_t *entry =
      (odin_server_runtime_entry_t *)calloc(1, sizeof(*entry));
#if defined(ODIN_SERVER_RUNTIME_TESTING)
  if (rt->fail_next_entry_alloc_armed) {
    rt->fail_next_entry_alloc_armed = 0;
    if (entry != NULL) {
      free(entry);
      entry = NULL;
    }
  }
#endif
  if (entry == NULL) {
    odin_server_session_destroy(ss);
    return;
  }
  entry->ss = ss;
  entry->prev = NULL;
  entry->next = rt->head;
  if (rt->head != NULL) {
    rt->head->prev = entry;
  }
  rt->head = entry;
  odin_server_session_set_dial_filter(ss, rt->dial_filter,
                                      rt->dial_filter_ud);
}

static void runtime_on_accept_error(odin_accept_loop_t *al, int err,
                                    void *user_data) {
  (void)al;
  odin_server_runtime_t *rt = (odin_server_runtime_t *)user_data;
  rt->terminal = 1;
  const odin_server_runtime_error_cb cb = rt->on_runtime_error;
  void *const cb_ud = rt->user_data;
  rt->in_callback = 1;
  cb(rt, err, cb_ud);
  if (rt->destroy_pending) {
    finish_destroy(rt);
    return;
  }
  rt->in_callback = 0;
}

static void runtime_on_close(odin_server_session_t *ss, int err,
                             void *user_data) {
  (void)err;
  odin_server_runtime_t *rt = (odin_server_runtime_t *)user_data;
  odin_server_runtime_entry_t *e = rt->head;
  while (e != NULL) {
    if (e->ss == ss) {
      if (e->prev != NULL) {
        e->prev->next = e->next;
      } else {
        rt->head = e->next;
      }
      if (e->next != NULL) {
        e->next->prev = e->prev;
      }
      free(e);
      odin_server_session_destroy(ss);
      return;
    }
    e = e->next;
  }
}

static void finish_destroy(odin_server_runtime_t *rt) {
  while (rt->head != NULL) {
    odin_server_runtime_entry_t *entry = rt->head;
    rt->head = entry->next;
    if (rt->head != NULL) {
      rt->head->prev = NULL;
    }
    odin_server_session_t *ss = entry->ss;
    free(entry);
    odin_server_session_destroy(ss);
  }
  if (rt->al != NULL) {
    odin_accept_loop_destroy(rt->al);
    rt->al = NULL;
  }
  free(rt);
}

#if defined(ODIN_SERVER_RUNTIME_TESTING)
int odin_server_runtime_test_inflight_count(const odin_server_runtime_t *rt) {
  if (rt == NULL) {
    return 0;
  }
  int count = 0;
  const odin_server_runtime_entry_t *e = rt->head;
  while (e != NULL) {
    count += 1;
    e = e->next;
  }
  return count;
}

int odin_server_runtime_test_is_terminal(const odin_server_runtime_t *rt) {
  return (rt != NULL && rt->terminal != 0) ? 1 : 0;
}

odin_accept_loop_t *
odin_server_runtime_test_get_accept_loop(odin_server_runtime_t *rt) {
  return rt->al;
}

int odin_server_runtime_test_fail_next_session_create(
    odin_server_runtime_t *rt) {
  rt->fail_next_session_create_armed = 1;
  return 0;
}

int odin_server_runtime_test_fail_next_entry_alloc(odin_server_runtime_t *rt) {
  rt->fail_next_entry_alloc_armed = 1;
  return 0;
}
#endif
