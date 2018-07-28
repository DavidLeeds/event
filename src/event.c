/*
 * Copyright (c) 2016-2018 David Leeds <davidesleeds@gmail.com>
 *
 * Event is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>

#include "event.h"

#ifndef EVENT_NOASSERT
#include <assert.h>
#define EVENT_ASSERT(expr)              assert(expr)
#else
#define EVENT_ASSERT(expr)
#endif

#define EVENT_EPOLL_READ                (EPOLLIN | EPOLLPRI)
#define EVENT_EPOLL_WRITE               (EPOLLOUT)
#ifdef EPOLLRDHUP    /* Since Linux 2.6.17 */
#define EVENT_EPOLL_HANGUP              (EPOLLERR | EPOLLHUP | EPOLLRDHUP)
#else
#define EVENT_EPOLL_HANGUP              (EPOLLERR | EPOLLHUP)
#endif

/*
 * Maximum number of events to process at a time with event_poll().  Remaining
 * events will be processed by a subsequent call.
 */
#define EVENT_MAXEVENTS                 32

/*
 * State for a dispatch event.
 */
struct event_dispatch {
    void (*handler)(void *);
    void *handler_arg;
};


/*
 * Convert an event type mask to epoll events.
 */
static uint32_t event_mask_to_epoll_events(uint32_t events)
{
    uint32_t mask = 0;

    if (events & EVENT_READ) {
        mask |= EVENT_EPOLL_READ;
    }
    if (events & EVENT_WRITE) {
        mask |= EVENT_EPOLL_WRITE;
    }
    if (events & EVENT_HANGUP) {
        mask |= EVENT_EPOLL_HANGUP;
    }
    return mask;
}

/*
 * Convert epoll events to an event type mask.
 */
static uint32_t event_mask_from_epoll_events(uint32_t events)
{
    uint32_t mask = 0;

    if (events & EVENT_EPOLL_READ) {
        mask |= EVENT_READ;
    }
    if (events & EVENT_EPOLL_WRITE) {
        mask |= EVENT_WRITE;
    }
    if (events & EVENT_EPOLL_HANGUP) {
        mask |= EVENT_HANGUP;
    }
    return mask;
}

/*
 * Set a flag to allow event_run() to return on the next event loop iteration.
 */
static void event_stop_handler(void *arg)
{
    struct event_context *ctx = (struct event_context *)arg;

    ctx->stop = true;
}

/*
 * Handle a dispatched callback on the event thread.
 */
static void event_dispatch_handler(struct event_io *io, uint32_t event_mask,
        void *arg)
{
    struct event_dispatch dispatch;
    int fd = event_io_fd(io);

    while (recv(fd, &dispatch, sizeof(dispatch), 0) == sizeof(dispatch)) {
        dispatch.handler(dispatch.handler_arg);
    }
}

/*
 * Register the event state to receive dispatched callbacks.
 * Returns 0 on success, or -errno on failure.
 */
static int event_register_dispatch_handler(struct event_context *ctx)
{
    int fds[2];
    int r;

    event_io_init(ctx, &ctx->dispatch_listener, event_dispatch_handler, NULL);
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) < 0) {
        return -errno;
    }
    /* Set the receiving file descriptor to non-blocking mode */
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
    /* Listen for dispatch messages */
    r = event_io_register(&ctx->dispatch_listener, fds[1], EVENT_READ);
    if (r < 0) {
        close(fds[0]);
        close(fds[1]);
        return r;
    }
    ctx->dispatch_fd = fds[0];
    return 0;
}

/*
 * Cleanup the dispatch handler.
 */
static void event_unregister_dispatch_handler(struct event_context *ctx)
{
    int fd;

    if (ctx->dispatch_fd == -1) {
        return;
    }
    close(ctx->dispatch_fd);
    ctx->dispatch_fd = -1;
    fd = ctx->dispatch_listener.fd;
    event_io_unregister(&ctx->dispatch_listener);
    close(fd);
}

/*
 * Insert a node into the timer list in chronological order for the specified
 * timeout time.
 */
static void event_timer_insert(struct event_timer *t, uint64_t time_ms)
{
    struct event_timer **prev;
    struct event_timer *node;

    t->time_ms = time_ms;

    /*
     * An elegant sorted insert is beyond the capabilities of the standard
     * sys/queue.h macros, so it is being done manually, here.
     */
    for (prev = &LIST_FIRST(&t->ctx->timer_list); (node = *prev) != NULL;
            prev = &LIST_NEXT(node, entry)) {
        if (t->time_ms < node->time_ms) {
            break;
        }
    }
    /* Doubly linked list insert before node */
    *prev = t;
    t->entry.le_prev = prev;
    t->entry.le_next = node;
    if (node) {
        node->entry.le_prev = &t->entry.le_next;
    }
}

/*
 * Evaluate all active timers.  This may be called at any time, but it is
 * optimal to call it once per scheduled timeout.
 * The return value is the number of milliseconds until the next timer event,
 * or -1 if no timeouts are scheduled.
 */
static int64_t event_timer_poll(struct event_context *ctx)
{
    struct event_timer *t;
    uint64_t cur_time_ms = 0;

    EVENT_ASSERT(ctx != NULL);

    while ((t = LIST_FIRST(&ctx->timer_list)) !=  NULL) {
        /*
         * Return if the next timeout is in the future, but re-check
         * the current time in case of long handler run time.
         */
        if (t->time_ms > cur_time_ms) {
            cur_time_ms = event_monotonic_ms();
            if (t->time_ms > cur_time_ms) {
                return t->time_ms - cur_time_ms;
            }
        }
        /* Pop first timer off queue and fire it */
        LIST_REMOVE(t, entry);
        if (t->repeat_ms) {
            /* Periodic timer: schedule next event */
            event_timer_insert(t, t->time_ms + t->repeat_ms);
        } else {
            /* One-shot timer: clear it */
            t->time_ms = 0;
        }
        t->handler(t, t->handler_arg);
    }
    return -1;
}

/*
 * Block waiting for the next I/O event or timer timeout.
 * Returns 0 on a normal event, signal interrupt, or timeout, and -errno
 * for an unrecoverable error.
 */
static int event_poll(struct event_context *ctx)
{
    struct epoll_event epoll_buf[EVENT_MAXEVENTS];
    struct epoll_event *e;
    struct event_io *io;
    uint32_t event_mask;
    int r;

    EVENT_ASSERT(ctx != NULL);

    r = epoll_wait(ctx->epoll_fd, epoll_buf, EVENT_MAXEVENTS,
            event_timer_poll(ctx));
    if (r > 0) {
        /* One or more events to process */
        for (e = epoll_buf; e < &epoll_buf[r]; ++e) {
            io = e->data.ptr;
            event_mask = event_mask_from_epoll_events(e->events);
            if (!event_mask) {
                /* Received event we don't care about */
                continue;
            }
            io->handler(io, event_mask, io->handler_arg);
        }
        return 0;
    }
    if (r < 0 && errno != EINTR) {
        /* Unexpected error */
        return -errno;
    }
    /* Timeout or signal interruption */
    return 0;
}

/*
 * Initialize an event context.
 * Returns 0 on success, or -errno on failure.
 */
int event_init(struct event_context *ctx)
{
    EVENT_ASSERT(ctx != NULL);

    /* Identify the event thread to detect actions from other threads */
    ctx->thread = pthread_self();

    /* Initialize I/O listener */
    /* XXX using older epoll_create() to support legacy toolchains */
    ctx->epoll_fd = epoll_create(1);
    if (ctx->epoll_fd < 0) {
        return -errno;
    }
    LIST_INIT(&ctx->io_list);

    /* Initialize timer */
    LIST_INIT(&ctx->timer_list);

    /* Setup thread-safe poll signaling mechanism */
    event_register_dispatch_handler(ctx);

    /* Initialize exit flag */
    ctx->stop = false;

    return 0;
}

/*
 * Free resources associated with an event context.  Any remaining registered
 * event handlers are unregistered automatically.
 */
void event_cleanup(struct event_context *ctx)
{
    struct event_io *io;
    struct event_timer *t;

    EVENT_ASSERT(ctx != NULL);

    /* Close dispatch sockets */
    event_unregister_dispatch_handler(ctx);

    /* Cancel any active timers */
    LIST_FOREACH(t, &ctx->timer_list, entry) {
        t->time_ms = 0;
        t->repeat_ms = 0;
    }
    LIST_INIT(&ctx->timer_list);

    /* Unregister file event listeners */
    while ((io = LIST_FIRST(&ctx->io_list)) != NULL) {
        event_io_unregister(io);
    }
    close(ctx->epoll_fd);
}

/*
 * Start the event loop.  This function blocks indefinitely until event_stop()
 * is called or an unrecoverable error occurs.
 * Returns -errno on error, and 0 on a normal exit.
 */
int event_run(struct event_context *ctx)
{
    int r;

    EVENT_ASSERT(ctx != NULL);

    /* Update in case event_init() was called on a different thread */
    ctx->thread = pthread_self();

    /* Event loop */
    while (!ctx->stop) {
        r = event_poll(ctx);
        if (r < 0) {
            return r;
        }
    }
    /* Clear stop flag in to allow another call to event_run(), if needed */
    ctx->stop = false;
    return 0;
}

/*
 * Thread and signal-safe mechanism to signal event_run() to return.
 * Returns 0 on success, or -errno on failure.
 */
int event_stop(const struct event_context *ctx)
{
    return event_dispatch(ctx, event_stop_handler, (void *)ctx);
}

/*
 * Thread and signal-safe mechanism to invoke a function on the event thread,
 * Returns 0 on success, or -errno on failure.
 */
int event_dispatch(const struct event_context *ctx,
        void (*handler)(void *), void *arg)
{
    struct event_dispatch dispatch = { handler, arg };

    EVENT_ASSERT(ctx != NULL);
    EVENT_ASSERT(handler != NULL);

    /*
     * Queue a callback to be executed by the dispatch handler on the main loop
     * using a local socket, which is thread and signal-safe. If the event
     * thread is dispatching its own callbacks, send in non-blocking mode to
     * fail if the socket buffer is full, instead of deadlocking the event
     * thread.  Other threads will block if they dispatch more callbacks than
     * the dispatch socket buffer can accept.
     */
    if (send(ctx->dispatch_fd, &dispatch, sizeof(dispatch), MSG_NOSIGNAL |
            (pthread_equal(ctx->thread, pthread_self())) ? MSG_DONTWAIT : 0)
            != sizeof(dispatch)) {
        return -errno;
    }
    return 0;
}

/*
 * Initialize an I/O event listener structure and associate it with an event
 * context.
 */
void event_io_init(struct event_context *ctx, struct event_io *io,
        void (*handler)(struct event_io *, uint32_t, void *), void *arg)
{
    EVENT_ASSERT(io != NULL);
    EVENT_ASSERT(handler != NULL);

    io->ctx = ctx;
    io->handler = handler;
    io->handler_arg = arg;
    io->fd = -1;
}

/*
 * Register to get event callbacks for an I/O file descriptor.  This may be
 * called with an already-registered file descriptor to modify the events to
 * listen for.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_register(struct event_io *io, int fd, uint32_t event_mask)
{
    struct epoll_event event = {
        .events = event_mask_to_epoll_events(event_mask),
        .data.ptr = io
    };

    EVENT_ASSERT(io != NULL);
    EVENT_ASSERT(io->ctx != NULL);

    if (io->fd != -1) {
        if (io->fd != fd) {
            /* Listener is already registered to a different FD */
            return -EEXIST;
        }
        /* Modify an existing listener's events */
        if (epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_MOD, io->fd, &event) < 0) {
            return -errno;
        }
        return 0;
    }
    /* Add a new listener */
    if (epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        return -errno;
    }
    io->fd = fd;
    LIST_INSERT_HEAD(&io->ctx->io_list, io, entry);
    return 0;
}

/*
 * Unregister an I/O event listener.
 * Returns 0 on success, or -errno on failure.
 */
int event_io_unregister(struct event_io *io)
{
    int r = 0;

    EVENT_ASSERT(io != NULL);

    if (io->fd == -1) {
        return 0;
    }
    LIST_REMOVE(io, entry);
    /*
     * Note: event arg is ignored for EPOLL_CTL_DEL, but Linux kernel < 2.6.9
     * required a non-NULL argument.  Passing in 1 for portability.
     */
    if (epoll_ctl(io->ctx->epoll_fd, EPOLL_CTL_DEL, io->fd, (void *)1) < 0) {
        r = -errno;
    }
    io->fd = -1;
    return r;
}

/*
 * Return the event listener's file descriptor.
 */
int event_io_fd(const struct event_io *io)
{
    return io ? io->fd : -1;
}

/*
 * Search for a registered event listener by file descriptor.
 */
struct event_io *event_io_find_by_fd(const struct event_context *ctx,
        int fd)
{
    struct event_io *listener;

    EVENT_ASSERT(ctx != NULL);

    LIST_FOREACH(listener, &ctx->io_list, entry) {
        if (listener->fd == fd) {
            return listener;
        }
    }
    return NULL;
}

/*
 * Initialize a timer structure and associate it with an event context.  Set
 * the timeout handler and an optional user-specified handler argument.
 */
void event_timer_init(struct event_context *ctx, struct event_timer *t,
        void (*handler)(struct event_timer *, void *), void *arg)
{
    EVENT_ASSERT(ctx != NULL);
    EVENT_ASSERT(t != NULL);
    EVENT_ASSERT(handler != NULL);

    t->ctx = ctx;
    t->time_ms = 0;
    t->repeat_ms = 0;
    t->handler = handler;
    t->handler_arg = arg;
}

/*
 * Set a timer with the specified delay (in milliseconds).  If periodic is
 * true, the timer will repeat indefinitely.  Otherwise, it will run once.
 * For periodic timers, long handler execution times will not skew the timeout
 * period, unless the handler does not return before the start of the next
 * period.
 */
void event_timer_set(struct event_timer *t, uint64_t ms, bool periodic)
{
    EVENT_ASSERT(t != NULL);
    EVENT_ASSERT(t->ctx != NULL);
    EVENT_ASSERT(!periodic || ms > 0);

    event_timer_cancel(t);
    event_timer_insert(t, event_monotonic_ms() + ms);
    if (periodic) {
        t->repeat_ms = ms;
    }
}

/*
 * Stop a timer.
 */
void event_timer_cancel(struct event_timer *t)
{
    EVENT_ASSERT(t != NULL);

    t->repeat_ms = 0;
    if (!t->time_ms) {
        return;
    }
    /* Remove timer from queue */
    t->time_ms = 0;
    LIST_REMOVE(t, entry);
}

/*
 * Return the number of milliseconds before the timer fires, or -1 if it is not
 * set.
 */
int64_t event_timer_delay_ms(const struct event_timer* t)
{
    uint64_t cur_time_ms;

    EVENT_ASSERT(t != NULL);

    if (!t->time_ms) {
        /* Timer not set */
        return -1;
    }
    cur_time_ms = event_monotonic_ms();
    if (cur_time_ms >= t->time_ms) {
        /* Trigger time is now */
        return 0;
    }
    return t->time_ms - cur_time_ms;
}

/*
 * Return true if a timeout is scheduled.
 */
bool event_timer_active(const struct event_timer *t)
{
    EVENT_ASSERT(t != NULL);

    return t->time_ms != 0;
}

/*
 * Return the number of milliseconds elapsed since boot.  This is is used
 * internally to calculate timer timeout times.
 */
uint64_t event_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
        EVENT_ASSERT(0);
    }
    return ((uint64_t)now.tv_sec) * 1000 + (uint64_t)(now.tv_nsec / 1000000);
}
