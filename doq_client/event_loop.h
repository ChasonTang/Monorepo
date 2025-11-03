/**
 * Simple Event Loop Implementation
 * Supports kqueue (macOS/BSD) and select (fallback)
 */

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>
#include <sys/time.h>

/* Event loop context (opaque) */
typedef struct event_loop event_loop_t;

/* Timer callback function type */
typedef void (*timer_callback_t)(void *user_data);

/* Socket read callback function type */
typedef void (*socket_callback_t)(int fd, void *user_data);

/**
 * Create a new event loop
 * @return  Event loop instance, or NULL on error
 */
event_loop_t *event_loop_create(void);

/**
 * Destroy an event loop
 * @param loop  Event loop instance
 */
void event_loop_destroy(event_loop_t *loop);

/**
 * Add a socket to monitor for read events
 * @param loop      Event loop instance
 * @param fd        Socket file descriptor
 * @param callback  Callback function to call when socket is readable
 * @param user_data User data to pass to callback
 * @return          0 on success, -1 on error
 */
int event_loop_add_socket(event_loop_t *loop, int fd, 
                          socket_callback_t callback, void *user_data);

/**
 * Remove a socket from the event loop
 * @param loop  Event loop instance
 * @param fd    Socket file descriptor
 * @return      0 on success, -1 on error
 */
int event_loop_remove_socket(event_loop_t *loop, int fd);

/**
 * Set a one-shot timer
 * @param loop          Event loop instance
 * @param timeout_us    Timeout in microseconds
 * @param callback      Callback function to call when timer expires
 * @param user_data     User data to pass to callback
 * @return              0 on success, -1 on error
 */
int event_loop_set_timer(event_loop_t *loop, uint64_t timeout_us,
                         timer_callback_t callback, void *user_data);

/**
 * Cancel the current timer
 * @param loop  Event loop instance
 */
void event_loop_cancel_timer(event_loop_t *loop);

/**
 * Run the event loop
 * @param loop  Event loop instance
 * @return      0 on success, -1 on error
 */
int event_loop_run(event_loop_t *loop);

/**
 * Stop the event loop
 * @param loop  Event loop instance
 */
void event_loop_stop(event_loop_t *loop);

/**
 * Get current monotonic timestamp in microseconds
 * @return  Current timestamp
 */
uint64_t event_loop_now_us(void);

#endif /* EVENT_LOOP_H */

