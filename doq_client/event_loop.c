/**
 * Simple Event Loop Implementation
 * Supports kqueue (macOS/BSD) and select (fallback)
 */

#include "event_loop.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#ifdef __APPLE__
#include <sys/event.h>
#define USE_KQUEUE 1
#else
#include <sys/select.h>
#define USE_SELECT 1
#endif

#define MAX_SOCKETS 16

typedef struct {
    int fd;
    socket_callback_t callback;
    void *user_data;
} socket_entry_t;

struct event_loop {
    int running;
    
    /* Socket management */
    socket_entry_t sockets[MAX_SOCKETS];
    int socket_count;
    
    /* Timer management */
    int timer_active;
    uint64_t timer_expire_us;
    timer_callback_t timer_callback;
    void *timer_user_data;
    
#ifdef USE_KQUEUE
    int kq;
#endif
};

/**
 * Get current monotonic timestamp in microseconds
 */
uint64_t event_loop_now_us(void) {
#ifdef __APPLE__
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/**
 * Create a new event loop
 */
event_loop_t *event_loop_create(void) {
    event_loop_t *loop = calloc(1, sizeof(event_loop_t));
    if (!loop) {
        return NULL;
    }
    
#ifdef USE_KQUEUE
    loop->kq = kqueue();
    if (loop->kq < 0) {
        perror("kqueue");
        free(loop);
        return NULL;
    }
#endif
    
    loop->running = 0;
    loop->socket_count = 0;
    loop->timer_active = 0;
    
    return loop;
}

/**
 * Destroy an event loop
 */
void event_loop_destroy(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
#ifdef USE_KQUEUE
    if (loop->kq >= 0) {
        close(loop->kq);
    }
#endif
    
    free(loop);
}

/**
 * Add a socket to monitor for read events
 */
int event_loop_add_socket(event_loop_t *loop, int fd,
                          socket_callback_t callback, void *user_data) {
    if (!loop || fd < 0 || !callback) {
        return -1;
    }
    
    if (loop->socket_count >= MAX_SOCKETS) {
        fprintf(stderr, "Too many sockets\n");
        return -1;
    }
    
    /* Check if socket already exists */
    for (int i = 0; i < loop->socket_count; i++) {
        if (loop->sockets[i].fd == fd) {
            /* Update existing entry */
            loop->sockets[i].callback = callback;
            loop->sockets[i].user_data = user_data;
            return 0;
        }
    }
    
    /* Add new socket */
    loop->sockets[loop->socket_count].fd = fd;
    loop->sockets[loop->socket_count].callback = callback;
    loop->sockets[loop->socket_count].user_data = user_data;
    loop->socket_count++;
    
#ifdef USE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(loop->kq, &ev, 1, NULL, 0, NULL) < 0) {
        perror("kevent add");
        loop->socket_count--;
        return -1;
    }
#endif
    
    return 0;
}

/**
 * Remove a socket from the event loop
 */
int event_loop_remove_socket(event_loop_t *loop, int fd) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    /* Find and remove socket */
    for (int i = 0; i < loop->socket_count; i++) {
        if (loop->sockets[i].fd == fd) {
#ifdef USE_KQUEUE
            struct kevent ev;
            EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
            kevent(loop->kq, &ev, 1, NULL, 0, NULL);
#endif
            
            /* Shift remaining sockets */
            memmove(&loop->sockets[i], &loop->sockets[i + 1],
                    (loop->socket_count - i - 1) * sizeof(socket_entry_t));
            loop->socket_count--;
            return 0;
        }
    }
    
    return -1;
}

/**
 * Set a one-shot timer
 */
int event_loop_set_timer(event_loop_t *loop, uint64_t timeout_us,
                         timer_callback_t callback, void *user_data) {
    if (!loop || !callback) {
        return -1;
    }
    
    loop->timer_active = 1;
    loop->timer_expire_us = event_loop_now_us() + timeout_us;
    loop->timer_callback = callback;
    loop->timer_user_data = user_data;
    
    return 0;
}

/**
 * Cancel the current timer
 */
void event_loop_cancel_timer(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    loop->timer_active = 0;
}

/**
 * Stop the event loop
 */
void event_loop_stop(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    loop->running = 0;
}

#ifdef USE_KQUEUE
/**
 * Run the event loop (kqueue version)
 */
int event_loop_run(event_loop_t *loop) {
    if (!loop) {
        return -1;
    }
    
    loop->running = 1;
    printf("[EventLoop] Starting event loop with %d sockets\n", loop->socket_count);
    
    while (loop->running) {
        struct kevent events[MAX_SOCKETS];
        int nevents;
        struct timespec timeout;
        struct timespec *timeout_ptr = NULL;
        
        /* Calculate timeout */
        if (loop->timer_active) {
            uint64_t now = event_loop_now_us();
            uint64_t remaining_us;
            if (now >= loop->timer_expire_us) {
                /* Timer already expired */
                timeout.tv_sec = 0;
                timeout.tv_nsec = 0;
                remaining_us = 0;
            } else {
                remaining_us = loop->timer_expire_us - now;
                timeout.tv_sec = remaining_us / 1000000ULL;
                timeout.tv_nsec = (remaining_us % 1000000ULL) * 1000ULL;
            }
            timeout_ptr = &timeout;
            printf("[EventLoop] Timer set for %llu us\n", (unsigned long long)remaining_us);
        } else {
            printf("[EventLoop] No timer, will wait indefinitely\n");
        }
        
        /* Wait for events */
        nevents = kevent(loop->kq, NULL, 0, events, MAX_SOCKETS, timeout_ptr);
        
        if (nevents < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("kevent wait");
            return -1;
        }
        
        printf("[EventLoop] kevent returned %d events\n", nevents);
        
        /* Process socket events */
        for (int i = 0; i < nevents; i++) {
            if (events[i].filter == EVFILT_READ) {
                int fd = events[i].ident;
                printf("[EventLoop] Read event on fd %d\n", fd);
                
                /* Find socket callback */
                for (int j = 0; j < loop->socket_count; j++) {
                    if (loop->sockets[j].fd == fd) {
                        loop->sockets[j].callback(fd, loop->sockets[j].user_data);
                        break;
                    }
                }
            }
        }
        
        /* Check timer */
        if (loop->timer_active) {
            uint64_t now = event_loop_now_us();
            if (now >= loop->timer_expire_us) {
                timer_callback_t cb = loop->timer_callback;
                void *user_data = loop->timer_user_data;
                loop->timer_active = 0;
                printf("[EventLoop] Timer expired, calling callback\n");
                cb(user_data);
            }
        }
    }
    
    return 0;
}
#endif

#ifdef USE_SELECT
/**
 * Run the event loop (select version)
 */
int event_loop_run(event_loop_t *loop) {
    if (!loop) {
        return -1;
    }
    
    loop->running = 1;
    
    while (loop->running) {
        fd_set readfds;
        int maxfd = -1;
        struct timeval timeout;
        struct timeval *timeout_ptr = NULL;
        
        /* Build fd_set */
        FD_ZERO(&readfds);
        for (int i = 0; i < loop->socket_count; i++) {
            FD_SET(loop->sockets[i].fd, &readfds);
            if (loop->sockets[i].fd > maxfd) {
                maxfd = loop->sockets[i].fd;
            }
        }
        
        /* Calculate timeout */
        if (loop->timer_active) {
            uint64_t now = event_loop_now_us();
            if (now >= loop->timer_expire_us) {
                /* Timer already expired */
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
            } else {
                uint64_t remaining_us = loop->timer_expire_us - now;
                timeout.tv_sec = remaining_us / 1000000ULL;
                timeout.tv_usec = remaining_us % 1000000ULL;
            }
            timeout_ptr = &timeout;
        }
        
        /* Wait for events */
        int nready = select(maxfd + 1, &readfds, NULL, NULL, timeout_ptr);
        
        if (nready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            return -1;
        }
        
        /* Process socket events */
        if (nready > 0) {
            for (int i = 0; i < loop->socket_count; i++) {
                if (FD_ISSET(loop->sockets[i].fd, &readfds)) {
                    loop->sockets[i].callback(loop->sockets[i].fd,
                                             loop->sockets[i].user_data);
                }
            }
        }
        
        /* Check timer */
        if (loop->timer_active) {
            uint64_t now = event_loop_now_us();
            if (now >= loop->timer_expire_us) {
                timer_callback_t cb = loop->timer_callback;
                void *user_data = loop->timer_user_data;
                loop->timer_active = 0;
                cb(user_data);
            }
        }
    }
    
    return 0;
}
#endif

