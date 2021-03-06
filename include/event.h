/*
 * Copyright (c) 2016-2020 David Leeds <davidesleeds@gmail.com>
 *
 * Event is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <pthread.h>

struct epoll_event;
#if defined(EVENT_LIBSYSTEMD)
struct sd_event;
struct sd_event_source;
#endif /* EVENT_LIBSYSTEMD */

/*
 * Define EVENT_NOASSERT to compile out all assertions used internally.
 */
// #define EVENT_NOASSERT

/*
 * One-hot encoded I/O event types.
 */
enum {
    EVENT_IO_READ               = 0x01,
    EVENT_IO_WRITE              = 0x02,
    EVENT_IO_DISCONNECT         = 0x04,
    EVENT_IO_PRIORITY           = 0x08
};

struct event_context;

/*
 * State for an I/O event listener.
 */
struct event_io {
    struct event_context *ctx;
    int fd;
    uint32_t event_mask;
    void (*handler)(struct event_io *, uint32_t, void *);
    void *handler_arg;
    LIST_ENTRY(event_io) entry;
};

/*
 * State for a timer.
 */
struct event_timer {
    struct event_context *ctx;
    uint64_t time_ms;
    uint64_t repeat_ms;
    void (*handler)(struct event_timer *, void *);
    void *handler_arg;
    LIST_ENTRY(event_timer) entry;
};

/*
 * State for an event context that manages all registered events for a given
 * event thread.
 */
struct event_context {
    pthread_t thread;
    int epoll_fd;
    struct epoll_event *epoll_pending;
    size_t epoll_pending_len;
    LIST_HEAD(, event_io) io_list;
    int dispatch_fd;
    struct event_io dispatch_listener;
    LIST_HEAD(, event_timer) timer_list;
    bool stop;

#if defined(EVENT_LIBSYSTEMD)
    struct sd_event *sd_event;
    struct sd_event_source *sd_epoll;
    struct sd_event_source *sd_timer;
#endif /* EVENT_LIBSYSTEMD */
};


/*
 * Initialize an event context.
 * Returns 0 on success, or -errno on failure.
 */
int event_init(struct event_context *ctx);

/*
 * Free resources associated with an event context.  Any remaining registered
 * event handlers are unregistered automatically.
 */
void event_cleanup(struct event_context *ctx);

/*
 * Start the event loop.  This function blocks indefinitely until event_stop()
 * is called or an unrecoverable error occurs.
 * Returns -errno on error, and 0 on a normal exit.
 */
int event_run(struct event_context *ctx);

/*
 * Thread and signal-safe mechanism to signal event_run() to return.
 * Returns 0 on success, or -errno on failure.
 */
int event_stop(struct event_context *ctx);

/*
 * Thread and signal-safe mechanism to invoke a function on the event thread,
 * Returns 0 on success, or -errno on failure.
 */
int event_dispatch(const struct event_context *ctx,
        void (*handler)(void *), void *arg);

/*
 * Initialize an I/O event listener structure and associate it with an event
 * context.
 */
void event_io_init(struct event_context *ctx, struct event_io *io,
        void (*handler)(struct event_io *, uint32_t, void *), void *arg);

/*
 * Register to get event callbacks for an I/O file descriptor.  This may be
 * called with an already-registered file descriptor to modify the events to
 * listen for.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_register(struct event_io *io, int fd, uint32_t event_mask);

/*
 * Unregister an I/O event listener.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_unregister(struct event_io *io);

/*
 * Unregister an I/O event listener and close the associated file descriptor.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_close(struct event_io *io);

/*
 * Return the event listener's file descriptor.
 */
int event_io_fd(const struct event_io *io);

/*
 * Search for a registered event listener by file descriptor.
 */
struct event_io *event_io_find_by_fd(const struct event_context *ctx,
        int fd);

/*
 * Return true if the file descriptor is readable.
 */
static inline bool event_io_is_read(uint32_t event_mask)
{
    return event_mask & EVENT_IO_READ;
}

/*
 * Return true if the file descriptor is writable.
 */
static inline bool event_io_is_write(uint32_t event_mask)
{
    return event_mask & EVENT_IO_WRITE;
}

/*
 * Return true if there was an error or hang-up event on a socket.
 */
static inline bool event_io_is_disconnect(uint32_t event_mask)
{
    return event_mask & EVENT_IO_DISCONNECT;
}

/*
 * Initialize a timer structure and associate it with an event context.  Set
 * the timeout handler and an optional user-specified handler argument.
 */
void event_timer_init(struct event_context *ctx, struct event_timer *t,
        void (*handler)(struct event_timer *, void *), void *arg);

/*
 * Set a timer with the specified interval (in milliseconds).  If periodic is
 * true, the timer will repeat indefinitely.  Otherwise, it will run once.
 *
 * For periodic timers, long handler execution times will not skew the timeout
 * period, although entire intervals may be dropped, if they occur in the past.
 */
void event_timer_set(struct event_timer *t, uint64_t interval_ms,
        bool periodic);

/*
 * Set a timer that fires at the specified absolute time (in milliseconds since
 * boot on the monotonic clock).  If repeat_ms is non-zero, the timer will
 * repeat indefinitely with the specified interval.  Otherwise, it will run
 * once.
 *
 * Specifying a start time in the past will result in the timer firing
 * immediately.
 *
 * For periodic timers, long handler execution times will not skew the timeout
 * period, although entire intervals may be dropped, if they occur in the past.
 */
void event_timer_set_abs(struct event_timer *t, uint64_t start_ms,
        uint64_t repeat_ms);

/*
 * Stop a timer.
 */
void event_timer_cancel(struct event_timer *t);

/*
 * Return the number of milliseconds before the timer fires, or -1 if it is not
 * set.
 */
int64_t event_timer_delay_ms(const struct event_timer *t);

/*
 * Return true if a timeout is scheduled.
 */
bool event_timer_active(const struct event_timer *t);

/*
 * Return the number of milliseconds elapsed since boot.  This is is used
 * internally to calculate timer timeout times.
 */
uint64_t event_monotonic_ms(void);

#if defined(EVENT_LIBSYSTEMD)

/*
 * Attach an event context to an existing sd-event loop.  This allows the
 * event interface to be integrated with an application already using the
 * sd-event library.
 * Returns 0 on success, or -errno on failure.
 */
int event_attach_sdevent(struct event_context *ctx, struct sd_event *e);

/*
 * Detach an event context from an sd-event loop.
 */
void event_detach_sdevent(struct event_context *ctx);

#endif /* EVENT_LIBSYSTEMD */
