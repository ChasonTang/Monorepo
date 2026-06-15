#include "odin/event_loop.h"

#if defined(ODIN_EVENT_LOOP_TESTING)
#include "odin/event_loop_internal_test.h"
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#else
#error "odin/event_loop supports only macOS kqueue and Linux epoll"
#endif

typedef struct odin_event_task_t odin_event_task_t;
typedef struct odin_timer_heap_entry_t odin_timer_heap_entry_t;
typedef struct odin_ready_item_t odin_ready_item_t;
#if defined(ODIN_EVENT_LOOP_TESTING) && defined(__APPLE__)
typedef struct odin_kqueue_registered_t odin_kqueue_registered_t;
#endif

struct odin_event_io_t {
  odin_event_loop_t *loop;
  int fd;
  unsigned int events;
  odin_event_io_cb cb;
  void *user_data;
  int active;
  unsigned int generation;
  uint64_t sequence;
  int deferred;
  odin_event_io_t *next;
  odin_event_io_t *deferred_next;
};

struct odin_event_timer_t {
  odin_event_loop_t *loop;
  uint64_t due_us;
  uint64_t repeat_us;
  odin_event_timer_cb cb;
  void *user_data;
  int active;
  unsigned int generation;
  uint64_t sequence;
  size_t heap_refs;
  int deferred;
  odin_event_timer_t *next;
  odin_event_timer_t *deferred_next;
};

struct odin_event_task_t {
  odin_event_task_cb cb;
  void *user_data;
  odin_event_task_t *next;
};

struct odin_timer_heap_entry_t {
  odin_event_timer_t *timer;
  uint64_t due_us;
  unsigned int generation;
  uint64_t sequence;
};

struct odin_ready_item_t {
  odin_event_io_t *io;
  unsigned int generation;
  unsigned int events;
  uint64_t sequence;
};

#if defined(ODIN_EVENT_LOOP_TESTING) && defined(__APPLE__)
struct odin_kqueue_registered_t {
  int fd;
  unsigned int events;
  odin_kqueue_registered_t *next;
};
#endif

struct odin_event_loop_t {
  pthread_t owner;
  int running;
  int stop_requested;
  int backend_fd;
#if defined(__linux__)
  int timer_fd;
#endif
  odin_event_io_t *io_head;
  odin_event_io_t *deferred_io_head;
  odin_event_timer_t *timer_head;
  odin_event_timer_t *deferred_timer_head;
  odin_event_task_t *task_head;
  odin_event_task_t *task_tail;
  odin_timer_heap_entry_t *timer_heap;
  size_t timer_heap_len;
  size_t timer_heap_cap;
  uint64_t next_io_sequence;
  uint64_t next_timer_sequence;
  size_t snapshot_depth;
#if defined(ODIN_EVENT_LOOP_TESTING)
  int use_fake_now;
  uint64_t fake_now_us;
  int fail_next_backend_wait_err;
  int fail_next_timer_start_err;
  odin_event_loop_test_ready_t *queued_backend_events;
  size_t queued_backend_count;
#if defined(__APPLE__)
  int fail_kqueue_change;
  unsigned int fail_kqueue_event;
  int fail_kqueue_err;
  odin_kqueue_registered_t *kqueue_registered;
#endif
#endif
};

#if defined(ODIN_EVENT_LOOP_TESTING)
static size_t g_live_loops;
static size_t g_live_ios;
static size_t g_live_timers;
static size_t g_live_tasks;
static int g_fail_next_backend_create_err;
#endif

static void assert_owner(odin_event_loop_t *loop) {
  assert(loop != NULL);
  assert(pthread_equal(pthread_self(), loop->owner));
}

static int valid_input_mask(unsigned int events) {
  return events != 0 && (events & ~(ODIN_EVENT_READ | ODIN_EVENT_WRITE)) == 0;
}

#if defined(__APPLE__)
static void set_cloexec(int fd) {
  const int flags = fcntl(fd, F_GETFD, 0);
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
}
#endif

static uint64_t monotonic_us(odin_event_loop_t *loop) {
#if defined(ODIN_EVENT_LOOP_TESTING)
  if (loop->use_fake_now) {
    return loop->fake_now_us;
  }
#endif
  (void)loop;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void enter_dispatch_snapshot(odin_event_loop_t *loop) {
  loop->snapshot_depth += 1;
}

static void free_io_handle(odin_event_io_t *io) {
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_ios -= 1;
#endif
  free(io);
}

static void remove_timer_from_loop(odin_event_loop_t *loop,
                                   odin_event_timer_t *timer) {
  odin_event_timer_t **pp = &loop->timer_head;
  while (*pp != NULL && *pp != timer) {
    pp = &(*pp)->next;
  }
  if (*pp == timer) {
    *pp = timer->next;
  }
}

static void free_timer_handle(odin_event_timer_t *timer) {
  odin_event_loop_t *loop = timer->loop;
  remove_timer_from_loop(loop, timer);
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_timers -= 1;
#endif
  free(timer);
}

static void reclaim_deferred(odin_event_loop_t *loop) {
  while (loop->deferred_io_head != NULL) {
    odin_event_io_t *io = loop->deferred_io_head;
    loop->deferred_io_head = io->deferred_next;
    free_io_handle(io);
  }
  while (loop->deferred_timer_head != NULL) {
    odin_event_timer_t *timer = loop->deferred_timer_head;
    loop->deferred_timer_head = timer->deferred_next;
    free_timer_handle(timer);
  }
}

static void leave_dispatch_snapshot(odin_event_loop_t *loop) {
  assert(loop->snapshot_depth > 0);
  loop->snapshot_depth -= 1;
  if (loop->snapshot_depth == 0) {
    reclaim_deferred(loop);
  }
}

static void retire_io_handle(odin_event_loop_t *loop, odin_event_io_t *io) {
  if (loop->snapshot_depth == 0) {
    free_io_handle(io);
    return;
  }
  if (!io->deferred) {
    io->deferred = 1;
    io->deferred_next = loop->deferred_io_head;
    loop->deferred_io_head = io;
  }
}

static void retire_timer_handle(odin_event_loop_t *loop,
                                odin_event_timer_t *timer) {
  if (timer->heap_refs > 0) {
    return;
  }
  if (loop->snapshot_depth == 0) {
    free_timer_handle(timer);
    return;
  }
  if (!timer->deferred) {
    timer->deferred = 1;
    timer->deferred_next = loop->deferred_timer_head;
    loop->deferred_timer_head = timer;
  }
}

static void release_timer_heap_ref(odin_event_loop_t *loop,
                                   odin_event_timer_t *timer) {
  assert(timer->heap_refs > 0);
  timer->heap_refs -= 1;
  if (!timer->active) {
    retire_timer_handle(loop, timer);
  }
}

static int timer_heap_less(const odin_timer_heap_entry_t *a,
                           const odin_timer_heap_entry_t *b) {
  if (a->due_us != b->due_us) {
    return a->due_us < b->due_us;
  }
  return a->sequence < b->sequence;
}

static int timer_heap_prepare_push(odin_event_loop_t *loop) {
  if (loop->timer_heap_len < loop->timer_heap_cap) {
    return 0;
  }
  const size_t old_cap = loop->timer_heap_cap;
  const size_t new_cap = old_cap == 0 ? 8 : old_cap * 2;
  if (new_cap < old_cap) {
    errno = ENOMEM;
    return -1;
  }
  odin_timer_heap_entry_t *new_heap = (odin_timer_heap_entry_t *)realloc(
      loop->timer_heap, new_cap * sizeof(new_heap[0]));
  if (new_heap == NULL) {
    errno = ENOMEM;
    return -1;
  }
  loop->timer_heap = new_heap;
  loop->timer_heap_cap = new_cap;
  return 0;
}

static void timer_heap_push_prepared(odin_event_loop_t *loop,
                                     odin_event_timer_t *timer) {
  size_t i = loop->timer_heap_len++;
  odin_timer_heap_entry_t entry = {
      timer,
      timer->due_us,
      timer->generation,
      timer->sequence,
  };
  timer->heap_refs += 1;
  while (i > 0) {
    const size_t parent = (i - 1) / 2;
    if (timer_heap_less(&loop->timer_heap[parent], &entry)) {
      break;
    }
    loop->timer_heap[i] = loop->timer_heap[parent];
    i = parent;
  }
  loop->timer_heap[i] = entry;
}

static odin_timer_heap_entry_t timer_heap_pop_min(odin_event_loop_t *loop) {
  const odin_timer_heap_entry_t min = loop->timer_heap[0];
  const odin_timer_heap_entry_t last = loop->timer_heap[--loop->timer_heap_len];
  size_t i = 0;
  while (loop->timer_heap_len > 0) {
    const size_t left = i * 2 + 1;
    const size_t right = left + 1;
    if (left >= loop->timer_heap_len) {
      break;
    }
    size_t child = left;
    if (right < loop->timer_heap_len &&
        timer_heap_less(&loop->timer_heap[right], &loop->timer_heap[left])) {
      child = right;
    }
    if (timer_heap_less(&last, &loop->timer_heap[child])) {
      break;
    }
    loop->timer_heap[i] = loop->timer_heap[child];
    i = child;
  }
  if (loop->timer_heap_len > 0) {
    loop->timer_heap[i] = last;
  }
  return min;
}

static int first_live_timer_deadline(odin_event_loop_t *loop,
                                     uint64_t *out_due_us) {
  while (loop->timer_heap_len > 0) {
    const odin_timer_heap_entry_t entry = loop->timer_heap[0];
    odin_event_timer_t *timer = entry.timer;
    if (timer->active && entry.generation == timer->generation) {
      *out_due_us = entry.due_us;
      return 1;
    }
    (void)timer_heap_pop_min(loop);
    release_timer_heap_ref(loop, timer);
  }
  return 0;
}

static int append_task(odin_event_loop_t *loop, odin_event_task_cb cb,
                       void *user_data) {
  odin_event_task_t *task = (odin_event_task_t *)calloc(1, sizeof(*task));
  if (task == NULL) {
    errno = ENOMEM;
    return -1;
  }
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_tasks += 1;
#endif
  task->cb = cb;
  task->user_data = user_data;
  if (loop->task_tail != NULL) {
    loop->task_tail->next = task;
  } else {
    loop->task_head = task;
  }
  loop->task_tail = task;
  return 0;
}

static void free_task(odin_event_task_t *task) {
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_tasks -= 1;
#endif
  free(task);
}

static void cancel_queued_tasks(odin_event_loop_t *loop) {
  odin_event_task_t *task = loop->task_head;
  loop->task_head = NULL;
  loop->task_tail = NULL;
  while (task != NULL) {
    odin_event_task_t *next = task->next;
    free_task(task);
    task = next;
  }
}

static void drain_posted_tasks(odin_event_loop_t *loop) {
  odin_event_task_t *snapshot = loop->task_head;
  if (snapshot == NULL) {
    return;
  }
  loop->task_head = NULL;
  loop->task_tail = NULL;
  enter_dispatch_snapshot(loop);
  while (snapshot != NULL) {
    odin_event_task_t *next = snapshot->next;
    snapshot->cb(loop, snapshot->user_data);
    free_task(snapshot);
    snapshot = next;
  }
  leave_dispatch_snapshot(loop);
}

static void remove_io_from_active_list(odin_event_loop_t *loop,
                                       odin_event_io_t *io) {
  odin_event_io_t **pp = &loop->io_head;
  while (*pp != NULL && *pp != io) {
    pp = &(*pp)->next;
  }
  if (*pp == io) {
    *pp = io->next;
  }
}

static odin_event_io_t *find_active_io_by_fd(odin_event_loop_t *loop, int fd) {
  for (odin_event_io_t *io = loop->io_head; io != NULL; io = io->next) {
    if (io->active && io->fd == fd) {
      return io;
    }
  }
  return NULL;
}

#if defined(__linux__)
static uint32_t epoll_events_from_odin(unsigned int events) {
  uint32_t out = 0;
  if (events & ODIN_EVENT_READ) {
    out |= EPOLLIN;
  }
  if (events & ODIN_EVENT_WRITE) {
    out |= EPOLLOUT;
  }
#if defined(EPOLLRDHUP)
  out |= EPOLLRDHUP;
#endif
  return out;
}

static int backend_io_add(odin_event_io_t *io, unsigned int events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = epoll_events_from_odin(events);
  ev.data.ptr = io;
  return epoll_ctl(io->loop->backend_fd, EPOLL_CTL_ADD, io->fd, &ev);
}

static int backend_io_update(odin_event_io_t *io, unsigned int events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = epoll_events_from_odin(events);
  ev.data.ptr = io;
  return epoll_ctl(io->loop->backend_fd, EPOLL_CTL_MOD, io->fd, &ev);
}

static void backend_io_delete(odin_event_io_t *io) {
  (void)epoll_ctl(io->loop->backend_fd, EPOLL_CTL_DEL, io->fd, NULL);
}
#else
static int kqueue_filter_for_event(unsigned int event) {
  return event == ODIN_EVENT_READ ? EVFILT_READ : EVFILT_WRITE;
}

#if defined(ODIN_EVENT_LOOP_TESTING)
static int kqueue_change_kind_to_test_kind(int add) {
  return add ? ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD
             : ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE;
}

static odin_kqueue_registered_t *find_kqueue_registered(odin_event_loop_t *loop,
                                                        int fd) {
  for (odin_kqueue_registered_t *entry = loop->kqueue_registered; entry != NULL;
       entry = entry->next) {
    if (entry->fd == fd) {
      return entry;
    }
  }
  return NULL;
}

static int prepare_kqueue_registered_add(odin_event_loop_t *loop, int fd) {
  if (find_kqueue_registered(loop, fd) != NULL) {
    return 0;
  }
  odin_kqueue_registered_t *entry =
      (odin_kqueue_registered_t *)calloc(1, sizeof(*entry));
  if (entry == NULL) {
    errno = ENOMEM;
    return -1;
  }
  entry->fd = fd;
  entry->next = loop->kqueue_registered;
  loop->kqueue_registered = entry;
  return 0;
}

static void note_kqueue_registered_change(odin_event_loop_t *loop, int fd,
                                          int add, unsigned int event) {
  odin_kqueue_registered_t *entry = find_kqueue_registered(loop, fd);
  assert(entry != NULL || !add);
  if (entry == NULL) {
    return;
  }
  if (add) {
    entry->events |= event;
    return;
  }
  entry->events &= ~event;
  if (entry->events != 0) {
    return;
  }
  odin_kqueue_registered_t **pp = &loop->kqueue_registered;
  while (*pp != NULL && *pp != entry) {
    pp = &(*pp)->next;
  }
  if (*pp == entry) {
    *pp = entry->next;
  }
  free(entry);
}

static void free_kqueue_registered(odin_event_loop_t *loop) {
  while (loop->kqueue_registered != NULL) {
    odin_kqueue_registered_t *entry = loop->kqueue_registered;
    loop->kqueue_registered = entry->next;
    free(entry);
  }
}
#endif

static int backend_kqueue_change_one(odin_event_loop_t *loop, int fd, int add,
                                     unsigned int event, odin_event_io_t *io) {
#if defined(ODIN_EVENT_LOOP_TESTING)
  const int test_change = kqueue_change_kind_to_test_kind(add);
  if (loop->fail_kqueue_change == test_change &&
      loop->fail_kqueue_event == event) {
    const int err = loop->fail_kqueue_err;
    loop->fail_kqueue_change = 0;
    errno = err;
    return -1;
  }
  if (add && prepare_kqueue_registered_add(loop, fd) != 0) {
    return -1;
  }
#endif
  struct kevent change;
  struct kevent receipt;
  EV_SET(&change, (uintptr_t)fd, kqueue_filter_for_event(event),
         (uint16_t)((add ? EV_ADD : EV_DELETE) | EV_RECEIPT), 0, 0, io);
  int rc;
  do {
    rc = kevent(loop->backend_fd, &change, 1, &receipt, 1, NULL);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    return -1;
  }
  if (rc != 1) {
    errno = EIO;
    return -1;
  }
  if ((receipt.flags & EV_ERROR) != 0 && receipt.data != 0) {
    errno = (int)receipt.data;
    return -1;
  }
#if defined(ODIN_EVENT_LOOP_TESTING)
  note_kqueue_registered_change(loop, fd, add, event);
#endif
  return 0;
}

static int rollback_kqueue_filters(odin_event_loop_t *loop, int fd,
                                   unsigned int events, odin_event_io_t *io) {
  int first_err = 0;
  if (events & ODIN_EVENT_READ) {
    if (backend_kqueue_change_one(loop, fd, 0, ODIN_EVENT_READ, io) != 0 &&
        first_err == 0) {
      first_err = errno;
    }
  }
  if (events & ODIN_EVENT_WRITE) {
    if (backend_kqueue_change_one(loop, fd, 0, ODIN_EVENT_WRITE, io) != 0 &&
        first_err == 0) {
      first_err = errno;
    }
  }
  if (first_err != 0) {
    errno = first_err;
    return -1;
  }
  return 0;
}

static int backend_io_add(odin_event_io_t *io, unsigned int events) {
  unsigned int added = 0;
  if (events & ODIN_EVENT_READ) {
    if (backend_kqueue_change_one(io->loop, io->fd, 1, ODIN_EVENT_READ, io) !=
        0) {
      return -1;
    }
    added |= ODIN_EVENT_READ;
  }
  if (events & ODIN_EVENT_WRITE) {
    if (backend_kqueue_change_one(io->loop, io->fd, 1, ODIN_EVENT_WRITE, io) !=
        0) {
      const int err = errno;
      if (rollback_kqueue_filters(io->loop, io->fd, added, io) != 0) {
        abort();
      }
      errno = err;
      return -1;
    }
  }
  return 0;
}

static int backend_io_update(odin_event_io_t *io, unsigned int events) {
  const unsigned int old_events = io->events;
  const unsigned int to_add = events & ~old_events;
  const unsigned int to_delete = old_events & ~events;
  unsigned int added = 0;

  if (to_add & ODIN_EVENT_READ) {
    if (backend_kqueue_change_one(io->loop, io->fd, 1, ODIN_EVENT_READ, io) !=
        0) {
      return -1;
    }
    added |= ODIN_EVENT_READ;
  }
  if (to_add & ODIN_EVENT_WRITE) {
    if (backend_kqueue_change_one(io->loop, io->fd, 1, ODIN_EVENT_WRITE, io) !=
        0) {
      const int err = errno;
      if (rollback_kqueue_filters(io->loop, io->fd, added, io) != 0) {
        abort();
      }
      errno = err;
      return -1;
    }
    added |= ODIN_EVENT_WRITE;
  }
  if (to_delete & ODIN_EVENT_READ) {
    if (backend_kqueue_change_one(io->loop, io->fd, 0, ODIN_EVENT_READ, io) !=
        0) {
      const int err = errno;
      if (rollback_kqueue_filters(io->loop, io->fd, added, io) != 0) {
        abort();
      }
      errno = err;
      return -1;
    }
  }
  if (to_delete & ODIN_EVENT_WRITE) {
    if (backend_kqueue_change_one(io->loop, io->fd, 0, ODIN_EVENT_WRITE, io) !=
        0) {
      const int err = errno;
      if (rollback_kqueue_filters(io->loop, io->fd, added, io) != 0) {
        abort();
      }
      errno = err;
      return -1;
    }
  }
  return 0;
}

static void backend_io_delete(odin_event_io_t *io) {
  if (io->events & ODIN_EVENT_READ) {
    (void)backend_kqueue_change_one(io->loop, io->fd, 0, ODIN_EVENT_READ, io);
  }
  if (io->events & ODIN_EVENT_WRITE) {
    (void)backend_kqueue_change_one(io->loop, io->fd, 0, ODIN_EVENT_WRITE, io);
  }
}
#endif

static int ready_item_add(odin_ready_item_t **items, size_t *count, size_t *cap,
                          odin_event_io_t *io, unsigned int generation,
                          unsigned int events) {
  if (events == 0) {
    return 0;
  }
  for (size_t i = 0; i < *count; ++i) {
    if ((*items)[i].io == io && (*items)[i].generation == generation) {
      (*items)[i].events |= events;
      return 0;
    }
  }
  if (*count == *cap) {
    const size_t new_cap = *cap == 0 ? 8 : *cap * 2;
    if (new_cap < *cap) {
      errno = ENOMEM;
      return -1;
    }
    odin_ready_item_t *new_items =
        (odin_ready_item_t *)realloc(*items, new_cap * sizeof(new_items[0]));
    if (new_items == NULL) {
      errno = ENOMEM;
      return -1;
    }
    *items = new_items;
    *cap = new_cap;
  }
  (*items)[*count].io = io;
  (*items)[*count].generation = generation;
  (*items)[*count].events = events;
  (*items)[*count].sequence = io->sequence;
  *count += 1;
  return 0;
}

static void sort_ready_items(odin_ready_item_t *items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const odin_ready_item_t item = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].sequence > item.sequence) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = item;
  }
}

static void dispatch_ready_items(odin_event_loop_t *loop,
                                 odin_ready_item_t *items, size_t count) {
  sort_ready_items(items, count);
  enter_dispatch_snapshot(loop);
  for (size_t i = 0; i < count; ++i) {
    odin_event_io_t *io = items[i].io;
    if (io->active && io->generation == items[i].generation) {
      io->cb(loop, io, io->fd, items[i].events, io->user_data);
    }
  }
  leave_dispatch_snapshot(loop);
}

static int dispatch_due_timers(odin_event_loop_t *loop) {
  odin_timer_heap_entry_t *snapshot = NULL;
  size_t count = 0;
  size_t cap = 0;
  int rc = 0;

  enter_dispatch_snapshot(loop);
  const uint64_t pass_now = monotonic_us(loop);
  while (loop->timer_heap_len > 0) {
    odin_timer_heap_entry_t entry = loop->timer_heap[0];
    odin_event_timer_t *timer = entry.timer;
    if (!timer->active || entry.generation != timer->generation) {
      (void)timer_heap_pop_min(loop);
      release_timer_heap_ref(loop, timer);
      continue;
    }
    if (entry.due_us > pass_now) {
      break;
    }
    if (count == cap) {
      const size_t new_cap = cap == 0 ? 8 : cap * 2;
      odin_timer_heap_entry_t *new_snapshot =
          (odin_timer_heap_entry_t *)realloc(snapshot,
                                             new_cap * sizeof(snapshot[0]));
      if (new_snapshot == NULL) {
        errno = ENOMEM;
        rc = -1;
        goto done;
      }
      snapshot = new_snapshot;
      cap = new_cap;
    }
    entry = timer_heap_pop_min(loop);
    release_timer_heap_ref(loop, timer);
    snapshot[count++] = entry;
  }

  for (size_t i = 0; i < count; ++i) {
    odin_event_timer_t *timer = snapshot[i].timer;
    if (!timer->active || timer->generation != snapshot[i].generation) {
      continue;
    }
    const unsigned int fired_generation = timer->generation;
    timer->cb(loop, timer, timer->user_data);
    if (timer->active && timer->generation == fired_generation &&
        timer->repeat_us > 0) {
      const uint64_t now = monotonic_us(loop);
      if (UINT64_MAX - now < timer->repeat_us ||
          timer_heap_prepare_push(loop) != 0) {
        timer->active = 0;
        timer->generation += 1;
        retire_timer_handle(loop, timer);
        continue;
      }
      timer->due_us = now + timer->repeat_us;
      timer->generation += 1;
      timer->sequence = ++loop->next_timer_sequence;
      timer_heap_push_prepared(loop, timer);
    } else if (timer->active && timer->generation == fired_generation) {
      timer->active = 0;
      timer->generation += 1;
      retire_timer_handle(loop, timer);
    }
  }

done:
  free(snapshot);
  leave_dispatch_snapshot(loop);
  return rc;
}

#if defined(__linux__)
static int arm_timerfd(odin_event_loop_t *loop, int has_due, uint64_t due_us) {
  struct itimerspec its;
  memset(&its, 0, sizeof(its));
  if (has_due) {
    its.it_value.tv_sec = (time_t)(due_us / 1000000u);
    its.it_value.tv_nsec = (long)((due_us % 1000000u) * 1000u);
  }
  return timerfd_settime(loop->timer_fd, has_due ? TFD_TIMER_ABSTIME : 0, &its,
                         NULL);
}

static void drain_timerfd(int timer_fd) {
  uint64_t expirations;
  for (;;) {
    const ssize_t n = read(timer_fd, &expirations, sizeof(expirations));
    if (n == (ssize_t)sizeof(expirations)) {
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}
#endif

static int backend_wait(odin_event_loop_t *loop, int force_nonblocking) {
#if defined(ODIN_EVENT_LOOP_TESTING)
  if (loop->fail_next_backend_wait_err != 0) {
    const int err = loop->fail_next_backend_wait_err;
    loop->fail_next_backend_wait_err = 0;
    errno = err;
    return -1;
  }
  if (loop->queued_backend_events != NULL) {
    odin_ready_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    odin_event_loop_test_ready_t *entries = loop->queued_backend_events;
    const size_t entry_count = loop->queued_backend_count;
    loop->queued_backend_events = NULL;
    loop->queued_backend_count = 0;
    for (size_t i = 0; i < entry_count; ++i) {
      if (ready_item_add(&items, &count, &cap, entries[i].io,
                         entries[i].io->generation, entries[i].events) != 0) {
        free(entries);
        free(items);
        return -1;
      }
    }
    free(entries);
    dispatch_ready_items(loop, items, count);
    free(items);
    return 0;
  }
#endif

  uint64_t next_due = 0;
  const int has_due = first_live_timer_deadline(loop, &next_due);
#if defined(__linux__)
  if (arm_timerfd(loop, has_due, next_due) != 0) {
    return -1;
  }
  struct epoll_event events[64];
  const int timeout_ms = force_nonblocking ? 0 : -1;
  const int n = epoll_wait(loop->backend_fd, events, 64, timeout_ms);
  if (n < 0) {
    return -1;
  }
  odin_ready_item_t *items = NULL;
  size_t count = 0;
  size_t cap = 0;
  for (int i = 0; i < n; ++i) {
    odin_event_io_t *io = (odin_event_io_t *)events[i].data.ptr;
    if (io == NULL) {
      drain_timerfd(loop->timer_fd);
      continue;
    }
    unsigned int mask = 0;
    if (events[i].events & EPOLLIN) {
      mask |= ODIN_EVENT_READ;
    }
    if (events[i].events & EPOLLOUT) {
      mask |= ODIN_EVENT_WRITE;
    }
    if (events[i].events & (EPOLLERR | EPOLLHUP
#if defined(EPOLLRDHUP)
                            | EPOLLRDHUP
#endif
                            )) {
      mask |= ODIN_EVENT_ERROR;
    }
    if (ready_item_add(&items, &count, &cap, io, io->generation, mask) != 0) {
      free(items);
      return -1;
    }
  }
#else
  struct kevent events[64];
  struct timespec timeout;
  struct timespec *timeout_ptr = NULL;
  if (force_nonblocking) {
    timeout.tv_sec = 0;
    timeout.tv_nsec = 0;
    timeout_ptr = &timeout;
  } else if (has_due) {
    const uint64_t now = monotonic_us(loop);
    const uint64_t delta = next_due > now ? next_due - now : 0;
    timeout.tv_sec = (time_t)(delta / 1000000u);
    timeout.tv_nsec = (long)((delta % 1000000u) * 1000u);
    timeout_ptr = &timeout;
  }
  const int n = kevent(loop->backend_fd, NULL, 0, events, 64, timeout_ptr);
  if (n < 0) {
    return -1;
  }
  odin_ready_item_t *items = NULL;
  size_t count = 0;
  size_t cap = 0;
  for (int i = 0; i < n; ++i) {
    odin_event_io_t *io = (odin_event_io_t *)events[i].udata;
    if (io == NULL) {
      continue;
    }
    unsigned int mask = 0;
    if (events[i].filter == EVFILT_READ) {
      mask |= ODIN_EVENT_READ;
    } else if (events[i].filter == EVFILT_WRITE) {
      mask |= ODIN_EVENT_WRITE;
    }
    if ((events[i].flags & (EV_ERROR | EV_EOF)) != 0) {
      mask |= ODIN_EVENT_ERROR;
    }
    if (ready_item_add(&items, &count, &cap, io, io->generation, mask) != 0) {
      free(items);
      return -1;
    }
  }
#endif
  dispatch_ready_items(loop, items, count);
  free(items);
  return 0;
}

static int backend_create(odin_event_loop_t *loop) {
#if defined(__linux__)
  loop->backend_fd = epoll_create1(EPOLL_CLOEXEC);
  if (loop->backend_fd < 0) {
    return -1;
  }
  loop->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (loop->timer_fd < 0) {
    const int err = errno;
    close(loop->backend_fd);
    loop->backend_fd = -1;
    errno = err;
    return -1;
  }
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = NULL;
  if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, loop->timer_fd, &ev) != 0) {
    const int err = errno;
    close(loop->timer_fd);
    close(loop->backend_fd);
    loop->timer_fd = -1;
    loop->backend_fd = -1;
    errno = err;
    return -1;
  }
#else
  loop->backend_fd = kqueue();
  if (loop->backend_fd < 0) {
    return -1;
  }
  set_cloexec(loop->backend_fd);
#endif
  return 0;
}

static void backend_close(odin_event_loop_t *loop) {
#if defined(__linux__)
  if (loop->timer_fd >= 0) {
    close(loop->timer_fd);
    loop->timer_fd = -1;
  }
#endif
  if (loop->backend_fd >= 0) {
    close(loop->backend_fd);
    loop->backend_fd = -1;
  }
}

int odin_event_loop_create(odin_event_loop_t **out) {
  assert(out != NULL);
  odin_event_loop_t *loop = (odin_event_loop_t *)calloc(1, sizeof(*loop));
  if (loop == NULL) {
    errno = ENOMEM;
    return -1;
  }
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_loops += 1;
#endif
  loop->owner = pthread_self();
  loop->backend_fd = -1;
#if defined(__linux__)
  loop->timer_fd = -1;
#endif

#if defined(ODIN_EVENT_LOOP_TESTING)
  if (g_fail_next_backend_create_err != 0) {
    const int err = g_fail_next_backend_create_err;
    g_fail_next_backend_create_err = 0;
#if defined(ODIN_EVENT_LOOP_TESTING)
    g_live_loops -= 1;
#endif
    free(loop);
    errno = err;
    return -1;
  }
#endif

  if (backend_create(loop) != 0) {
    const int err = errno;
#if defined(ODIN_EVENT_LOOP_TESTING)
    g_live_loops -= 1;
#endif
    free(loop);
    errno = err;
    return -1;
  }
  *out = loop;
  return 0;
}

int odin_event_loop_run(odin_event_loop_t *loop) {
  assert_owner(loop);
  if (loop->running) {
    errno = EALREADY;
    return -1;
  }
  loop->running = 1;
  loop->stop_requested = 0;

  int rc = 0;
  int saved_errno = 0;
  while (!loop->stop_requested) {
    drain_posted_tasks(loop);
    if (loop->stop_requested) {
      break;
    }
    if (dispatch_due_timers(loop) != 0) {
      rc = -1;
      saved_errno = errno;
      break;
    }
    if (loop->stop_requested) {
      break;
    }
    const int force_nonblocking = loop->task_head != NULL;
    if (backend_wait(loop, force_nonblocking) != 0) {
      if (errno == EINTR) {
        continue;
      }
      rc = -1;
      saved_errno = errno;
      break;
    }
  }

  if (loop->stop_requested) {
    cancel_queued_tasks(loop);
  }
  if (loop->snapshot_depth == 0) {
    reclaim_deferred(loop);
  }
  loop->running = 0;
  if (rc != 0) {
    errno = saved_errno;
  }
  return rc;
}

void odin_event_loop_stop(odin_event_loop_t *loop) {
  assert_owner(loop);
  if (loop->running) {
    loop->stop_requested = 1;
  }
}

void odin_event_loop_destroy(odin_event_loop_t *loop) {
  if (loop == NULL) {
    return;
  }
  assert_owner(loop);
  assert(!loop->running);
  assert(loop->snapshot_depth == 0);

  while (loop->io_head != NULL) {
    odin_event_io_t *io = loop->io_head;
    loop->io_head = io->next;
    backend_io_delete(io);
    io->active = 0;
    io->generation += 1;
    free_io_handle(io);
  }

  while (loop->timer_heap_len > 0) {
    odin_timer_heap_entry_t entry = timer_heap_pop_min(loop);
    release_timer_heap_ref(loop, entry.timer);
  }
  while (loop->timer_head != NULL) {
    odin_event_timer_t *timer = loop->timer_head;
    loop->timer_head = timer->next;
#if defined(ODIN_EVENT_LOOP_TESTING)
    g_live_timers -= 1;
#endif
    free(timer);
  }

  cancel_queued_tasks(loop);
  reclaim_deferred(loop);
#if defined(ODIN_EVENT_LOOP_TESTING)
  free(loop->queued_backend_events);
#if defined(__APPLE__)
  free_kqueue_registered(loop);
#endif
#endif
  free(loop->timer_heap);
  backend_close(loop);
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_loops -= 1;
#endif
  free(loop);
}

int odin_event_io_start(odin_event_loop_t *loop, int fd, unsigned int events,
                        odin_event_io_cb cb, void *user_data,
                        odin_event_io_t **out) {
  assert_owner(loop);
  if (!valid_input_mask(events)) {
    errno = EINVAL;
    return -1;
  }
  if (find_active_io_by_fd(loop, fd) != NULL) {
    errno = EEXIST;
    return -1;
  }
  odin_event_io_t *io = (odin_event_io_t *)calloc(1, sizeof(*io));
  if (io == NULL) {
    errno = ENOMEM;
    return -1;
  }
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_ios += 1;
#endif
  io->loop = loop;
  io->fd = fd;
  io->events = events;
  io->cb = cb;
  io->user_data = user_data;
  io->active = 1;
  io->generation = 1;
  io->sequence = ++loop->next_io_sequence;
  if (backend_io_add(io, events) != 0) {
    const int err = errno;
    free_io_handle(io);
    errno = err;
    return -1;
  }
  io->next = loop->io_head;
  loop->io_head = io;
  *out = io;
  return 0;
}

int odin_event_io_update(odin_event_io_t *io, unsigned int events) {
  odin_event_loop_t *loop = io->loop;
  assert_owner(loop);
  if (!valid_input_mask(events)) {
    errno = EINVAL;
    return -1;
  }
  if (backend_io_update(io, events) != 0) {
    return -1;
  }
  io->events = events;
  io->generation += 1;
  return 0;
}

void odin_event_io_stop(odin_event_io_t *io) {
  const int saved_errno = errno;
  odin_event_loop_t *loop = io->loop;
  assert_owner(loop);
  if (io->active) {
    io->generation += 1;
    remove_io_from_active_list(loop, io);
    backend_io_delete(io);
    io->active = 0;
    retire_io_handle(loop, io);
  }
  errno = saved_errno;
}

int odin_event_timer_start(odin_event_loop_t *loop, uint64_t delay_us,
                           uint64_t repeat_us, odin_event_timer_cb cb,
                           void *user_data, odin_event_timer_t **out) {
  assert_owner(loop);
#if defined(ODIN_EVENT_LOOP_TESTING)
  if (loop->fail_next_timer_start_err != 0) {
    const int err = loop->fail_next_timer_start_err;
    loop->fail_next_timer_start_err = 0;
    errno = err;
    return -1;
  }
#endif
  const uint64_t now = monotonic_us(loop);
  if (UINT64_MAX - now < delay_us) {
    errno = EOVERFLOW;
    return -1;
  }
  if (timer_heap_prepare_push(loop) != 0) {
    return -1;
  }
  odin_event_timer_t *timer = (odin_event_timer_t *)calloc(1, sizeof(*timer));
  if (timer == NULL) {
    errno = ENOMEM;
    return -1;
  }
#if defined(ODIN_EVENT_LOOP_TESTING)
  g_live_timers += 1;
#endif
  timer->loop = loop;
  timer->due_us = now + delay_us;
  timer->repeat_us = repeat_us;
  timer->cb = cb;
  timer->user_data = user_data;
  timer->active = 1;
  timer->generation = 1;
  timer->sequence = ++loop->next_timer_sequence;
  timer->next = loop->timer_head;
  loop->timer_head = timer;
  timer_heap_push_prepared(loop, timer);
  *out = timer;
  return 0;
}

int odin_event_timer_reset(odin_event_timer_t *timer, uint64_t delay_us,
                           uint64_t repeat_us) {
  odin_event_loop_t *loop = timer->loop;
  assert_owner(loop);
  const uint64_t now = monotonic_us(loop);
  if (UINT64_MAX - now < delay_us) {
    errno = EOVERFLOW;
    return -1;
  }
  if (timer_heap_prepare_push(loop) != 0) {
    return -1;
  }
  timer->generation += 1;
  timer->due_us = now + delay_us;
  timer->repeat_us = repeat_us;
  timer->sequence = ++loop->next_timer_sequence;
  timer_heap_push_prepared(loop, timer);
  return 0;
}

void odin_event_timer_stop(odin_event_timer_t *timer) {
  const int saved_errno = errno;
  odin_event_loop_t *loop = timer->loop;
  assert_owner(loop);
  if (timer->active) {
    timer->active = 0;
    timer->generation += 1;
    retire_timer_handle(loop, timer);
  }
  errno = saved_errno;
}

int odin_event_post(odin_event_loop_t *loop, odin_event_task_cb cb,
                    void *user_data) {
  assert_owner(loop);
  return append_task(loop, cb, user_data);
}

#if defined(ODIN_EVENT_LOOP_TESTING)
void odin_event_loop_test_set_now_us(odin_event_loop_t *loop, uint64_t now_us) {
  assert_owner(loop);
  loop->use_fake_now = 1;
  loop->fake_now_us = now_us;
}

size_t odin_event_loop_test_live_timer_count(odin_event_loop_t *loop) {
  assert_owner(loop);
  uint64_t ignored_due;
  (void)first_live_timer_deadline(loop, &ignored_due);
  size_t count = 0;
  for (odin_event_timer_t *timer = loop->timer_head; timer != NULL;
       timer = timer->next) {
    if (timer->active) {
      count += 1;
    }
  }
  return count;
}

void odin_event_loop_test_reset_liveness(void) {
  assert(g_live_loops == 0);
  assert(g_live_ios == 0);
  assert(g_live_timers == 0);
  assert(g_live_tasks == 0);
  g_live_loops = 0;
  g_live_ios = 0;
  g_live_timers = 0;
  g_live_tasks = 0;
}

int odin_event_loop_test_liveness(odin_event_loop_test_liveness_t *out) {
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  out->loops = g_live_loops;
  out->io_handles = g_live_ios;
  out->timers = g_live_timers;
  out->task_nodes = g_live_tasks;
  return 0;
}

int odin_event_loop_test_fail_next_backend_create(int errnum) {
  if (errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  g_fail_next_backend_create_err = errnum;
  return 0;
}

int odin_event_loop_test_fail_next_backend_wait(odin_event_loop_t *loop,
                                                int errnum) {
  assert_owner(loop);
  if (errnum <= 0 || errnum == EINTR) {
    errno = EINVAL;
    return -1;
  }
  loop->fail_next_backend_wait_err = errnum;
  return 0;
}

int odin_event_loop_test_fail_next_timer_start(odin_event_loop_t *loop,
                                               int errnum) {
  assert_owner(loop);
  if (errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  loop->fail_next_timer_start_err = errnum;
  return 0;
}

int odin_event_loop_test_prepare_wait(odin_event_loop_t *loop,
                                      odin_event_loop_test_wait_record_t *out) {
  assert_owner(loop);
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
  memset(out, 0, sizeof(*out));
  uint64_t next_due = 0;
  const int has_due = first_live_timer_deadline(loop, &next_due);
#if defined(__linux__)
  out->backend = ODIN_EVENT_LOOP_TEST_BACKEND_LINUX;
  out->linux_timerfd.epoll_timeout_ms = -1;
  if (has_due) {
    out->linux_timerfd.armed = 1;
    out->linux_timerfd.abs_sec = (int64_t)(next_due / 1000000u);
    out->linux_timerfd.abs_nsec = (long)((next_due % 1000000u) * 1000u);
  }
#else
  out->backend = ODIN_EVENT_LOOP_TEST_BACKEND_MACOS;
  if (!has_due) {
    out->macos_kevent.timeout_is_null = 1;
  } else {
    const uint64_t now = monotonic_us(loop);
    const uint64_t delta = next_due > now ? next_due - now : 0;
    out->macos_kevent.timeout_is_null = 0;
    out->macos_kevent.rel_sec = (int64_t)(delta / 1000000u);
    out->macos_kevent.rel_nsec = (long)((delta % 1000000u) * 1000u);
  }
#endif
  return 0;
}

int odin_event_loop_test_backend_fds(odin_event_loop_t *loop,
                                     odin_event_loop_test_fd_record_t *out) {
  assert_owner(loop);
  if (out == NULL) {
    errno = EINVAL;
    return -1;
  }
#if defined(__linux__)
  out->backend = ODIN_EVENT_LOOP_TEST_BACKEND_LINUX;
  out->timer_fd = loop->timer_fd;
#else
  out->backend = ODIN_EVENT_LOOP_TEST_BACKEND_MACOS;
  out->timer_fd = -1;
#endif
  out->backend_fd = loop->backend_fd;
  return 0;
}

int odin_event_loop_test_fail_next_kqueue_change(odin_event_loop_t *loop,
                                                 int change, unsigned int event,
                                                 int errnum) {
  assert_owner(loop);
#if defined(__linux__)
  (void)change;
  (void)event;
  (void)errnum;
  errno = EOPNOTSUPP;
  return -1;
#else
  if ((change != ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_ADD &&
       change != ODIN_EVENT_LOOP_TEST_KQUEUE_CHANGE_DELETE) ||
      (event != ODIN_EVENT_READ && event != ODIN_EVENT_WRITE) || errnum <= 0) {
    errno = EINVAL;
    return -1;
  }
  loop->fail_kqueue_change = change;
  loop->fail_kqueue_event = event;
  loop->fail_kqueue_err = errnum;
  return 0;
#endif
}

int odin_event_loop_test_kqueue_registered_mask(odin_event_loop_t *loop, int fd,
                                                unsigned int *out_events) {
  assert_owner(loop);
#if defined(__linux__)
  (void)fd;
  (void)out_events;
  errno = EOPNOTSUPP;
  return -1;
#else
  if (out_events == NULL) {
    errno = EINVAL;
    return -1;
  }
  odin_kqueue_registered_t *entry = find_kqueue_registered(loop, fd);
  *out_events = entry != NULL ? entry->events : 0;
  return 0;
#endif
}

int odin_event_loop_test_dispatch_backend_events(
    odin_event_loop_t *loop, const odin_event_loop_test_ready_t *entries,
    size_t count) {
  assert_owner(loop);
  if (count > 0 && entries == NULL) {
    errno = EINVAL;
    return -1;
  }
  odin_ready_item_t *items = NULL;
  size_t item_count = 0;
  size_t item_cap = 0;
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].io == NULL || entries[i].io->loop != loop) {
      free(items);
      errno = EINVAL;
      return -1;
    }
    if (ready_item_add(&items, &item_count, &item_cap, entries[i].io,
                       entries[i].io->generation, entries[i].events) != 0) {
      free(items);
      return -1;
    }
  }
  dispatch_ready_items(loop, items, item_count);
  free(items);
  return 0;
}

int odin_event_loop_test_queue_backend_events(
    odin_event_loop_t *loop, const odin_event_loop_test_ready_t *entries,
    size_t count) {
  assert_owner(loop);
  if (loop->queued_backend_events != NULL) {
    errno = EEXIST;
    return -1;
  }
  if (count > 0 && entries == NULL) {
    errno = EINVAL;
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].io == NULL || entries[i].io->loop != loop) {
      errno = EINVAL;
      return -1;
    }
  }
  if (count == 0) {
    return 0;
  }
  loop->queued_backend_events =
      (odin_event_loop_test_ready_t *)calloc(count, sizeof(entries[0]));
  if (loop->queued_backend_events == NULL) {
    errno = ENOMEM;
    return -1;
  }
  memcpy(loop->queued_backend_events, entries, count * sizeof(entries[0]));
  loop->queued_backend_count = count;
  return 0;
}
#endif /* defined(ODIN_EVENT_LOOP_TESTING) */
